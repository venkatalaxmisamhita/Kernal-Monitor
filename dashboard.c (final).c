/*
 * Kernel Monitor - web dashboard, written entirely in C.
 *
 * Reads /proc, runs a small HTTP server on localhost and serves a live
 * dashboard page (embedded below) that shows every process with its
 * CPU, memory, disk I/O, threads, open files and more.
 *
 * Build:  gcc -O2 -pthread -o dashboard dashboard.c
 * Run:    ./dashboard            (then open http://localhost:8000)
 *         sudo ./dashboard       (full data for every process + syscall tracing)
 * Options: --port N   --no-browser
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define HIST 90
#define MAXP 4096
#define MAXEV 40
#define CPU_WARNING 70.0
#define CPU_CRITICAL 85.0
#define MEMORY_WARNING 75.0
#define MEMORY_CRITICAL 90.0
#define IO_WARNING 10.0
#define IO_CRITICAL 50.0
#define MAXCORES 256
#define NEED 3 /* samples in a row at CRITICAL before a process alert */

/* ------------------------------------------------------------------ data */

typedef struct {
    int used, seen, pid, hi, hn;
    char name[64], user[32], cmd[161], state;
    int threads, nice, fds;
    long ctx;
    unsigned long long prev_j, vsz, rss, io_r, io_w;
    double cpu, mem, cpu_time, age, io_t, r, w, rt, wt;
    int io_ok;
    char prev_state;
    int prev_threads, ppid, nchild, spike, spike_ttl, leak;
    unsigned long long start;
    float cpu_h[HIST], mem_h[HIST], r_h[HIST], w_h[HIST], rss_h[HIST];
} Proc;

typedef struct {
    char t[9];
    int kind, pid; /* kind: 0 exit, 1 start, 2 signal sent */
    char name[64], note[16];
} Event;

static Proc procs[MAXP];
static Event events[MAXEV];
static int nevents = 0;
static float sys_cpu_h[HIST], sys_mem_h[HIST];
static float sys_io_r_h[HIST], sys_io_w_h[HIST];
static int sys_n = 0;
static double g_cpu = 0, g_load[3] = {0, 0, 0}, g_up = 0;
static unsigned long long g_mem_total = 1, g_mem_used = 0;
static int g_ncpu = 1, g_port = 8000, self_pid = 0, parent_pid = 0;
static char g_token[33];
static char *g_page = NULL;
static size_t g_page_len = 0;
static double g_cpu_sum = 0, g_cpu_peak = 0, g_cpu_min = 101;
static double g_mem_sum = 0, g_mem_peak = 0, g_mem_min = 101;
static unsigned long g_cpu_cnt = 0, g_mem_cnt = 0;
static unsigned long long g_mem_free = 0, g_mem_avail = 0;
static double core_use[MAXCORES];
static int ncores = 0;
static double g_dr = 0, g_dw = 0, g_drops = 0, g_dwops = 0, g_io_peak = 0;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

static const char HTML[] =
    "<!doctype html>\n"
    "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>Kernel Monitor</title>\n"
    "<style>\n"
    ":root{--bg:#e8ecf0;--panel:#fbfcfd;--ink:#16202b;--mute:#5d6b7a;--line:#d3dae1;--grid:#e3e8ed;\n"
    "--cpu:#1f5fbf;--mem:#0f8f7a;--ok:#2a8a5c;--warn:#b86e00;--crit:#cf3a3a;\n"
    "--ui:\"Bahnschrift\",\"Segoe UI Variable\",\"Segoe UI\",system-ui,sans-serif;--mono:\"Cascadia Mono\",ui-monospace,Consolas,monospace}\n"
    "*{box-sizing:border-box}\n"
    "body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.4 var(--ui)}\n"
    "header{display:flex;flex-wrap:wrap;align-items:baseline;gap:8px 28px;padding:18px 28px 6px}\n"
    "h1{font-size:22px;font-weight:600;margin:0 auto 0 0;letter-spacing:.2px}\n"
    ".hs{color:var(--mute)}.hs b{color:var(--ink);font-weight:600;font-variant-numeric:tabular-nums}\n"
    "#conn{font-weight:600}#conn.off{color:var(--crit)}#conn.on{color:var(--ok)}\n"
    "main{display:grid;grid-template-columns:minmax(0,1.3fr) minmax(0,1fr);gap:16px;padding:12px 28px 28px}\n"
    ".panel{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:16px}\n"
    ".wide{grid-column:1/-1}\n"
    "h2{font-size:15px;font-weight:600;margin:0 0 10px}\n"
    ".sys{display:grid;grid-template-columns:200px minmax(0,1fr);gap:20px}\n"
    ".big{font-size:46px;font-weight:600;line-height:1;font-variant-numeric:tabular-nums}\n"
    ".big small{font-size:18px;color:var(--mute);font-weight:400}\n"
    ".k{color:var(--mute);margin-top:2px}.blk{margin-bottom:14px}\n"
    ".cv{width:100%;height:170px;display:block}.cv.s{height:54px}\n"
    ".tag{display:inline-block;padding:1px 8px;border-radius:10px;font-size:12px;font-weight:600;color:#fff}\n"
    ".ok{background:var(--ok)}.warn{background:var(--warn)}.crit{background:var(--crit)}\n"
    ".leg{display:flex;gap:16px;color:var(--mute);font-size:12px;margin-top:6px}\n"
    ".leg i{display:inline-block;width:14px;height:3px;vertical-align:middle;margin-right:5px}\n"
    ".tools{display:flex;gap:10px;align-items:center;margin-bottom:8px}.tools h2{margin:0 auto 0 0}\n"
    "input{font:inherit;padding:5px 9px;border:1px solid var(--line);border-radius:4px;background:#fff;width:170px}\n"
    ".tw{overflow:auto;max-height:430px;border:1px solid var(--line);border-radius:4px}\n"
    "table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}\n"
    "th{position:sticky;top:0;background:var(--panel);z-index:1;font-weight:600;text-align:left;color:var(--mute);padding:7px 8px;border-bottom:1px solid var(--line);cursor:pointer;white-space:nowrap;user-select:none}\n"
    "th.r,td.r{text-align:right}th.on{color:var(--ink)}\n"
    "td{padding:5px 8px;border-bottom:1px solid var(--grid);white-space:nowrap}\n"
    "tbody tr{cursor:pointer}tbody tr:hover{background:#f0f4f8}\n"
    "tr.sel{background:#e3edfb!important;box-shadow:inset 3px 0 0 var(--cpu)}\n"
    ".nm{max-width:170px;overflow:hidden;text-overflow:ellipsis;font-weight:600}.cm{max-width:300px;overflow:hidden;text-overflow:ellipsis;color:var(--mute);font-family:var(--mono);font-size:12px}\n"
    ".cmd{font-family:var(--mono);font-size:12px;color:var(--mute);word-break:break-all;margin-bottom:10px}\n"
    "button{font:inherit;font-weight:600;padding:6px 12px;border:1px solid var(--cpu);background:var(--cpu);color:#fff;border-radius:4px;cursor:pointer}button:hover{filter:brightness(1.1)}\n"
    ".tr td,.tr th{padding:3px 6px;cursor:default;position:static}\n"
    ".bar{display:inline-block;width:70px;height:7px;background:var(--grid);border-radius:4px;margin-right:8px;vertical-align:middle}\n"
    ".bar b{display:block;height:100%;border-radius:4px;background:var(--cpu)}\n"
    ".id{display:flex;flex-wrap:wrap;gap:4px 18px;color:var(--mute);margin-bottom:12px}.id b{color:var(--ink)}\n"
    "dl{display:grid;grid-template-columns:repeat(4,1fr);gap:4px;margin:6px 0 0}\n"
    "dt{color:var(--mute);font-size:12px}dd{margin:0;font-weight:600;font-variant-numeric:tabular-nums}\n"
    ".mh{display:flex;justify-content:space-between;align-items:baseline}.mh strong{font-size:22px}\n"
    "ul{list-style:none;margin:0;padding:0}\n"
    "li{padding:5px 0;border-bottom:1px solid var(--grid);display:flex;gap:10px;font-size:13px}\n"
    "li time{color:var(--mute);font-family:var(--mono);font-size:12px}\n"
    ".empty{color:var(--mute)}\n"
    "@media(max-width:980px){main{grid-template-columns:1fr;padding:12px 14px}header{padding:14px}.sys{grid-template-columns:1fr}}\n"
    ".res{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:24px}\n"
    "h3{font-size:13px;font-weight:600;margin:0 0 8px}.res h3{font-size:15px;margin:0}\n"
    ".g2{display:grid;grid-template-columns:repeat(2,1fr);gap:8px 14px;margin:12px 0 0}\n"
    ".cores{display:flex;gap:3px;align-items:flex-end;height:56px;margin-top:10px}\n"
    ".core{flex:1;max-width:26px;height:100%;background:var(--grid);border-radius:2px;display:flex;align-items:flex-end}\n"
    ".core b{display:block;width:100%;border-radius:2px}\n"
    ".bar2{height:8px;background:var(--grid);border-radius:4px;margin-top:8px}.bar2 b{display:block;height:100%;border-radius:4px;background:var(--mem)}\n"
    ".note{color:var(--mute);font-size:12px;margin-top:8px}\n"
    ".two{display:grid;grid-template-columns:minmax(0,1.5fr) minmax(0,1fr);gap:24px}\n"
    ".sh{margin-bottom:10px;gap:10px;flex-wrap:wrap}.sh h2{margin:0}\n"
    ".cond{border:1px solid var(--line);border-left:5px solid var(--ok);border-radius:4px;padding:10px 14px;margin-bottom:14px}\n"
    ".cond.c1{border-left-color:var(--warn)}.cond.c2{border-left-color:var(--crit)}\n"
    ".cond p{margin:4px 0 8px;font-size:16px;font-weight:600}.cond li{padding:3px 0}\n"
    ".cmp th{position:static;cursor:default}.cmp td{white-space:normal}.cmp .r{text-align:right}\n"
    ".badge{display:inline-block;color:#fff;font-weight:700;font-size:20px;padding:6px 18px;border-radius:6px;margin:10px 0 6px}\n"
    ".warnbox{background:#fff6e0;border:1px solid #f0d28a;border-radius:4px;padding:8px 12px;color:var(--ink);margin-bottom:10px}\n"
    "button.ghost{background:transparent;color:var(--cpu)}\n"
    "@media(max-width:980px){.res,.two{grid-template-columns:1fr}}\n"
    "button:disabled{opacity:.4;cursor:not-allowed}\n"
    "button.end{padding:2px 9px;font-size:12px;background:#fff;color:var(--crit);border-color:var(--crit)}\n"
    "button.end:hover:not(:disabled){background:var(--crit);color:#fff}\n"
    "button.danger{background:var(--crit);border-color:var(--crit)}\n"
    ".ch{white-space:nowrap}.ch.up{color:var(--crit)}.ch.dn{color:var(--ok)}\n"
    ".sp{font-family:var(--mono);letter-spacing:-1px;color:var(--cpu)}\n"
    ".fl{display:inline-block;font-size:11px;font-weight:600;border:1px solid var(--warn);color:var(--warn);border-radius:8px;padding:0 6px;margin-left:4px}\n"
    "li.na{justify-content:space-between;align-items:flex-start}.acts{display:flex;gap:6px}\n"
    ".md{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(16,24,34,.55);display:flex;align-items:center;justify-content:center;z-index:10}\n"
    ".md[hidden]{display:none}\n"
    ".mdc{background:var(--panel);border-radius:8px;padding:20px 22px;width:min(580px,92vw);max-height:88vh;overflow:auto}\n"
    ".mdc h2{font-size:18px}.opt{display:grid;grid-template-columns:200px 1fr;gap:12px;align-items:center;padding:9px 0;border-top:1px solid var(--grid)}\n"
    ".mdr{margin:12px 0;padding:8px 12px;border-radius:4px;background:#e8f0fb}.mda{margin-top:12px;text-align:right}\n"
    ".imp{margin:8px 0 12px}\n"
    "</style></head><body>\n"
    "<header><h1>Kernel Monitor</h1>\n"
    "<span class=\"hs\">Processes <b id=\"hc\">-</b></span><span class=\"hs\">Load <b id=\"hl\">-</b></span>\n"
    "<span class=\"hs\">Up <b id=\"hu\">-</b></span><span id=\"conn\" class=\"hs off\">connecting</span></header>\n"
    "<main>\n"
    "<section class=\"panel wide\"><div class=\"sys\">\n"
    " <div><div class=\"blk\"><div class=\"k\">System CPU</div><div class=\"big\" id=\"sc\">-</div><span id=\"sct\" class=\"tag ok\">Normal</span></div>\n"
    " <div class=\"blk\"><div class=\"k\">Memory</div><div class=\"big\" id=\"sm\">-</div><div class=\"k\" id=\"smd\"></div></div></div>\n"
    " <div><canvas id=\"chs\" class=\"cv\"></canvas>\n"
    " <div class=\"leg\"><span><i style=\"background:var(--cpu)\"></i>CPU %</span><span><i style=\"background:var(--mem)\"></i>Memory %</span><span>Dashed lines: CPU warning at 70% and critical at 85%. Last 90 seconds.</span></div></div>\n"
    "</div></section>\n"
    "<section class=\"panel wide\"><h2>System resources</h2>\n"
    "<div class=\"res\"><div id=\"rcpu\"></div><div id=\"rmem\"></div><div id=\"rio\"></div></div></section>\n"
    "<section class=\"panel wide\"><div class=\"tools\"><h2>All processes</h2><span class=\"hs\" id=\"shown\"></span>\n"
    "<input id=\"q\" placeholder=\"Filter by name, PID, user\"></div>\n"
    "<div class=\"tw\"><table><thead><tr id=\"th\"></tr></thead><tbody id=\"tb\"></tbody></table></div>\n"
    "<div class=\"empty\" style=\"margin-top:8px\">The change columns compare every process with its own sample from one second earlier (arrow up means it grew, arrow down means it shrank). Click a column to sort, click a row to monitor it, or press End to stop it. CPU is per core, so 100% means one full core. Disk and open-file values show n/a for other users' processes unless you start the dashboard with sudo.</div></section>\n"
    "<section class=\"panel wide\" id=\"selp\"><h2>Selected process</h2><div class=\"empty\">Waiting for data...</div></section>\n"
    "<section class=\"panel\" id=\"det\"></section>\n"
    "<aside style=\"display:grid;gap:16px;align-content:start\">\n"
    "<section class=\"panel\"><h2>Needs attention</h2><ul id=\"na\"></ul></section>\n"
    "<section class=\"panel\"><h2>Alerts</h2><ul id=\"al\"></ul></section>\n"
    "<section class=\"panel\"><h2>Process events</h2><ul id=\"ev\"></ul></section>\n"
    "</aside></main>\n"
    "<div class=\"md\" id=\"md\" hidden></div>\n"
    "<script>\n"
    "const $=s=>document.querySelector(s),H=90;\n"
    "const TOKEN=\"__TOKEN__\";\n"
    "let sel=null,sk=\"cpu\",sd=-1,last=null,md=null;\n"
    "const esc=s=>String(s).replace(/[&<>\"]/g,c=>({\"&\":\"&amp;\",\"<\":\"&lt;\",\">\":\"&gt;\",'\"':\"&quot;\"}[c]));\n"
    "let TH={cpu:[70,85],mem:[75,90],io:[10,50]},lost=null;\n"
    "const LV=[\"NORMAL\",\"WARNING\",\"CRITICAL\"],LC=[\"ok\",\"warn\",\"crit\"];\n"
    "const lv=(v,t)=>v>=t[1]?2:v>=t[0]?1:0;\n"
    "const st=(v,t)=>{const l=lv(v,t);return[LV[l],LC[l]]};\n"
    "const tagl=l=>`<span class=\"tag ${LC[l]}\">${LV[l]}</span>`;\n"
    "const lines=t=>[[t[0],\"--warn\"],[t[1],\"--crit\"]];\n"
    "const gb=b=>(b/1073741824).toFixed(2)+\" GB\";\n"
    "const mb=b=>b>=1073741824?(b/1073741824).toFixed(1)+\" GB\":(b/1048576).toFixed(0)+\" MB\";\n"
    "const dur=s=>{s=Math.floor(s);const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+\"d \":\"\")+h+\"h \"+m+\"m\"};\n"
    "const css=n=>getComputedStyle(document.documentElement).getPropertyValue(n).trim();\n"
    "function draw(cv,series,max,lines){\n"
    " const r=devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;cv.width=w*r;cv.height=h*r;\n"
    " const c=cv.getContext(\"2d\");c.scale(r,r);c.lineJoin=\"round\";\n"
    " const y=v=>h-2-(Math.min(v,max)/max)*(h-6);\n"
    " c.strokeStyle=css(\"--grid\");c.lineWidth=1;\n"
    " for(let g=0;g<=4;g++){const yy=Math.round(2+g*(h-6)/4)+.5;c.beginPath();c.moveTo(0,yy);c.lineTo(w,yy);c.stroke()}\n"
    " (lines||[]).forEach(([v,col])=>{if(v<=max){c.setLineDash([5,4]);c.strokeStyle=css(col);c.beginPath();c.moveTo(0,y(v));c.lineTo(w,y(v));c.stroke()}});c.setLineDash([]);\n"
    " series.forEach(s=>{const d=s.d;if(d.length<2)return;const x=i=>w-(d.length-1-i)*(w/(H-1));\n"
    "  c.beginPath();d.forEach((v,i)=>i?c.lineTo(x(i),y(v)):c.moveTo(x(i),y(v)));\n"
    "  if(s.fill){c.save();c.lineTo(x(d.length-1),h);c.lineTo(x(0),h);c.closePath();c.globalAlpha=.12;c.fillStyle=s.c;c.fill();c.restore();\n"
    "   c.beginPath();d.forEach((v,i)=>i?c.lineTo(x(i),y(v)):c.moveTo(x(i),y(v)))}\n"
    "  c.strokeStyle=s.c;c.lineWidth=s.fill?2:1.6;c.stroke()})}\n"
    "const stats=d=>{if(!d.length)return{avg:0,pk:0,mn:0,tr:\"Steady\"};\n"
    " const a=d.reduce((x,y)=>x+y,0)/d.length,n=d.length,rc=d.slice(-5),pv=d.slice(-10,-5);\n"
    " const m=x=>x.reduce((p,q)=>p+q,0)/(x.length||1),df=pv.length?m(rc)-m(pv):0;\n"
    " return{avg:a,pk:Math.max(...d),mn:Math.min(...d),tr:df>5?\"Rising\":df<-5?\"Falling\":\"Steady\"}};\n"
    "function block(label,val,unit,hist,color,id,max,th){\n"
    " const s=stats(hist),[t,cl]=st(val,th);\n"
    " return `<div class=\"blk\"><div class=\"mh\"><span class=\"k\">${label}</span><span class=\"tag ${cl}\">${t}</span></div>\n"
    " <div class=\"mh\"><strong>${val.toFixed(1)}${unit}</strong></div><canvas class=\"cv s\" id=\"${id}\" data-c=\"${color}\" data-m=\"${max}\"></canvas>\n"
    " <dl><div><dt>Average</dt><dd>${s.avg.toFixed(1)}</dd></div><div><dt>Peak</dt><dd>${s.pk.toFixed(1)}</dd></div>\n"
    " <div><dt>Minimum</dt><dd>${s.mn.toFixed(1)}</dd></div><div><dt>Trend</dt><dd>${s.tr}</dd></div></dl></div>`}\n"
    "const SN={R:\"Running\",S:\"Sleeping\",D:\"Disk wait\",Z:\"Zombie\",T:\"Stopped\",t:\"Traced\",I:\"Idle\",X:\"Dead\"};\n"
    "const COLS=[[\"pid\",\"PID\"],[\"name\",\"Name\"],[\"user\",\"User\"],[\"state\",\"State\"],[\"cpu\",\"CPU %\"],[\"dcpu\",\"CPU change\"],[\"mem\",\"Mem %\"],[\"dmem\",\"Memory change\"],[\"r\",\"Read MB/s\"],[\"w\",\"Write MB/s\"],[\"dio\",\"I/O change\"],[\"lvl\",\"Status\"],[\"sp\",\"Recent CPU\"],[\"rss\",\"RSS\"],[\"vsz\",\"Virtual\"],[\"threads\",\"Threads\"],[\"fds\",\"Open files\"],[\"cmd\",\"Command\"],[\"act\",\"\"]];\n"
    "const LEFT=[\"pid\",\"name\",\"user\",\"state\",\"cpu\",\"lvl\",\"sp\",\"cmd\",\"act\"];\n"
    "const ct=s=>s<60?s.toFixed(1)+\" s\":Math.floor(s/60)+\"m \"+Math.floor(s%60)+\"s\";\n"
    "const nv=(v,f)=>v===null||v===undefined||v<0?\"n/a\":f(v);\n"
    "let tr=null;\n"
    "$(\"#th\").innerHTML=COLS.map(([k,l])=>`<th data-k=\"${k}\" class=\"${LEFT.includes(k)?\"\":\"r\"} ${k===sk?\"on\":\"\"}\">${l}</th>`).join(\"\");\n"
    "const pv=a=>a.length>1?a[a.length-2]:(a.length?a[a.length-1]:0),cv=a=>a.length?a[a.length-1]:0;\n"
    "const trend=(dv,tol)=>dv>tol?\"Increasing\":dv<-tol?\"Decreasing\":\"Stable\";\n"
    "const AR={Increasing:\"&uarr; Increasing\",Decreasing:\"&darr; Decreasing\",Stable:\"&rarr; Stable\"};\n"
    "const sgn=(dv,dec,u)=>(dv>0?\"+\":dv<0?\"-\":\"\")+Math.abs(dv).toFixed(dec)+u;\n"
    "const chg=(dv,tol,dec,u)=>{const t=trend(dv,tol);return`<span class=\"ch ${t===\"Increasing\"?\"up\":t===\"Decreasing\"?\"dn\":\"\"}\">${t===\"Increasing\"?\"&uarr;\":t===\"Decreasing\"?\"&darr;\":\"&rarr;\"} ${sgn(dv,dec,u)}</span>`};\n"
    "const spark=a=>{const m=Math.max(20,...a);return a.map(v=>String.fromCharCode(9601+Math.min(7,Math.floor(Math.max(0,v)/m*8)))).join(\"\")};\n"
    "const flagsHtml=p=>(p.spike?'<span class=\"fl\">Spike</span>':\"\")+(p.leak?'<span class=\"fl\">Possible leak</span>':\"\")+(p.state===\"Z\"?'<span class=\"fl\">Zombie</span>':\"\")+(p.state===\"T\"?'<span class=\"fl\">Paused</span>':\"\");\n"
    "function derive(d){d.procs.forEach(p=>{const io=p.r===null?0:p.r+p.w;p.dcpu=p.cpu-p.pcpu;p.dmem=p.rss/1048576-p.prss;p.dio=p.r===null?0:io-p.pio;\n"
    " p.lvl=Math.max(lv(p.cpu,TH.cpu),lv(p.mem,TH.mem),p.r===null?0:lv(io,TH.io))})}\n"
    "function renderNA(d){\n"
    " const out=[];\n"
    " d.procs.forEach(p=>{const r=[],io=p.r===null?0:p.r+p.w;\n"
    "  if(lv(p.cpu,TH.cpu))r.push(`CPU ${p.cpu.toFixed(0)}%`);\n"
    "  if(lv(p.mem,TH.mem))r.push(`memory ${p.mem.toFixed(0)}% of RAM`);\n"
    "  if(p.r!==null&&lv(io,TH.io))r.push(`disk I/O ${io.toFixed(1)} MB/s`);\n"
    "  if(p.spike)r.push(\"CPU jumped far above its recent average\");\n"
    "  if(p.leak)r.push(\"memory keeps growing (possible leak)\");\n"
    "  if(p.state===\"Z\")r.push(\"zombie process\");\n"
    "  if(r.length)out.push({p,r,l:Math.max(p.lvl,(p.spike||p.leak||p.state===\"Z\")?1:0)})});\n"
    " out.sort((a,b)=>b.l-a.l||b.p.cpu-a.p.cpu);\n"
    " $(\"#na\").innerHTML=out.slice(0,6).map(({p,r,l})=>`<li class=\"na\"><div>${tagl(l)} <b>${esc(p.name)}</b> <span class=\"hs\">PID ${p.pid}</span><div class=\"note\">${r.join(\", \")}</div></div>\n"
    " <div class=\"acts\"><button class=\"ghost\" data-mon=\"${p.pid}\">Monitor</button><button class=\"end\" data-end=\"${p.pid}\" ${p.prot?\"disabled\":\"\"}>End</button></div></li>`).join(\"\")\n"
    " ||`<li class=\"empty\">Nothing needs attention. No process is at a warning level, spiking in CPU or growing in memory.</li>`}\n"
    "function openEnd(pid,msg){\n"
    " const p=last.procs.find(x=>x.pid===pid);if(!p)return;\n"
    " md={pid,start:p.start,msg:msg||\"\"};drawMd()}\n"
    "function closeMd(){md=null;$(\"#md\").hidden=true}\n"
    "function drawMd(){\n"
    " if(!md)return;const el=$(\"#md\");el.hidden=false;\n"
    " const p=last.procs.find(x=>x.pid===md.pid);\n"
    " const res=md.msg?`<div class=\"mdr\">${esc(md.msg)}</div>`:\"\";\n"
    " if(!p){el.innerHTML=`<div class=\"mdc\"><h2>Process ended</h2><div class=\"mdr\">${esc(md.msg||\"This process is no longer running.\")}</div><div class=\"mda\"><button onclick=\"closeMd()\">Close</button></div></div>`;return}\n"
    " const z=p.state===\"Z\",pa=p.state===\"T\";let opts;\n"
    " if(p.prot)opts=`<div class=\"warnbox\">This process is protected: it is the init process, this dashboard, the shell that started it, or a kernel thread. It cannot be ended here.</div>`;\n"
    " else if(z)opts=`<div class=\"warnbox\">This is a zombie. It has already finished and only waits for its parent (PID ${p.ppid}) to collect it, so it cannot be killed. Ending or restarting the parent clears it.</div>`;\n"
    " else opts=`<div class=\"opt\"><button class=\"danger\" onclick=\"sendSig('TERM')\">Terminate (SIGTERM)</button><span>Asks the process to shut down cleanly. Try this first.</span></div>\n"
    " <div class=\"opt\"><button class=\"danger\" onclick=\"sendSig('KILL')\">Force kill (SIGKILL)</button><span>The kernel ends it at once. It cannot clean up or save its data.</span></div>\n"
    " <div class=\"opt\"><button class=\"ghost\" onclick=\"sendSig('${pa?\"CONT\":\"STOP\"}')\">${pa?\"Resume (SIGCONT)\":\"Pause (SIGSTOP)\"}</button><span>${pa?\"Lets the paused process continue.\":\"Freezes the process without ending it. Its memory stays in use.\"}</span></div>`;\n"
    " const warn=(p.nchild>0?`<div class=\"note\">It has ${p.nchild} child process${p.nchild>1?\"es\":\"\"}, which may keep running without it.</div>`:\"\")+(p.user===\"root\"&&!p.prot?`<div class=\"note\">It is owned by root. Ending system services can break things.</div>`:\"\");\n"
    " el.innerHTML=`<div class=\"mdc\"><h2>End process: ${esc(p.name)} <span class=\"hs\">PID ${p.pid}</span></h2>\n"
    " <div class=\"id\"><span>User <b>${esc(p.user)}</b></span><span>State <b>${SN[p.state]||p.state}</b></span><span>Child processes <b>${p.nchild}</b></span></div>\n"
    " <div class=\"imp\">Ending it would free about <b>${(p.rss/1048576).toFixed(1)} MB</b> of memory and about <b>${p.cpu.toFixed(0)}%</b> of one CPU core (${(p.cpu/last.ncpu).toFixed(1)}% of total CPU capacity).</div>\n"
    " ${warn}${opts}${res}<div class=\"mda\"><button class=\"ghost\" onclick=\"closeMd()\">Close</button></div></div>`}\n"
    "async function sendSig(sig){\n"
    " if(!md)return;const pid=md.pid;md.msg=\"Sending...\";drawMd();\n"
    " try{const r=await fetch(`/api/signal?pid=${pid}&start=${md.start}&sig=${sig}`,{method:\"POST\",headers:{\"X-Token\":TOKEN}});md.msg=(await r.json()).message||\"Done.\"}\n"
    " catch(e){md.msg=\"The request failed. Is the dashboard still running?\"}\n"
    " if(md)drawMd();tick()}\n"
    "function renderRes(d){\n"
    " const s=d.sys,cs=s.cores||[],n=cs.length||d.ncpu,io=s.io_r+s.io_w;\n"
    " const act=cs.filter(x=>x>=10).length;let bi=0,li=0;\n"
    " cs.forEach((x,i)=>{if(x>cs[bi])bi=i;if(x<cs[li])li=i});\n"
    " const spread=cs.length?cs[bi]-cs[li]:0;\n"
    " const dist=cs.length<2?\"Single core\":spread<=20?\"Balanced\":spread<=50?\"Moderately uneven\":\"Unbalanced\";\n"
    " const col=x=>[\"cpu\",\"warn\",\"crit\"][lv(x,TH.cpu)];\n"
    " $(\"#rcpu\").innerHTML=`<div class=\"mh\"><h3>CPU</h3>${tagl(lv(s.cpu,TH.cpu))}</div><div class=\"big\">${s.cpu.toFixed(1)}<small>%</small></div>\n"
    " <div class=\"cores\">${cs.map((x,i)=>`<div class=\"core\" title=\"CPU ${i}: ${x.toFixed(0)}%\"><b style=\"height:${Math.max(2,x)}%;background:var(--${col(x)})\"></b></div>`).join(\"\")}</div>\n"
    " <dl class=\"g2\"><div><dt>Cores</dt><dd>${n}</dd></div><div><dt>Active cores (10% or more)</dt><dd>${act} of ${n}</dd></div>\n"
    " <div><dt>Busiest core</dt><dd>${cs.length?\"CPU \"+bi+\" at \"+cs[bi].toFixed(0)+\"%\":\"n/a\"}</dd></div><div><dt>Least busy core</dt><dd>${cs.length?\"CPU \"+li+\" at \"+cs[li].toFixed(0)+\"%\":\"n/a\"}</dd></div>\n"
    " <div><dt>Load distribution</dt><dd>${dist}</dd></div><div><dt>Gap busiest to least</dt><dd>${spread.toFixed(0)} points</dd></div>\n"
    " <div><dt>Average</dt><dd>${s.cpu_avg.toFixed(1)}%</dd></div><div><dt>Peak</dt><dd>${s.cpu_peak.toFixed(1)}%</dd></div><div><dt>Minimum</dt><dd>${s.cpu_min.toFixed(1)}%</dd></div></dl>\n"
    " <div class=\"note\">One bar per core. Warning at ${TH.cpu[0]}%, critical at ${TH.cpu[1]}%. Average, peak and minimum cover the time since the dashboard started.</div>`;\n"
    " $(\"#rmem\").innerHTML=`<div class=\"mh\"><h3>Memory</h3>${tagl(lv(s.mem_pct,TH.mem))}</div><div class=\"big\">${s.mem_pct.toFixed(1)}<small>%</small></div>\n"
    " <div class=\"bar2\"><b style=\"width:${s.mem_pct}%\"></b></div>\n"
    " <dl class=\"g2\"><div><dt>Total</dt><dd>${gb(s.mem_total)}</dd></div><div><dt>Used</dt><dd>${gb(s.mem_used)}</dd></div><div><dt>Available</dt><dd>${gb(s.mem_avail)}</dd></div><div><dt>Free</dt><dd>${gb(s.mem_free)}</dd></div>\n"
    " <div><dt>Current usage</dt><dd>${s.mem_pct.toFixed(1)}%</dd></div><div><dt>Average</dt><dd>${s.mem_avg.toFixed(1)}%</dd></div><div><dt>Peak</dt><dd>${s.mem_peak.toFixed(1)}%</dd></div><div><dt>Minimum</dt><dd>${s.mem_min.toFixed(1)}%</dd></div></dl>\n"
    " <div class=\"note\">Used is total minus available. Warning at ${TH.mem[0]}%, critical at ${TH.mem[1]}%.</div>`;\n"
    " $(\"#rio\").innerHTML=`<div class=\"mh\"><h3>Disk I/O</h3>${tagl(lv(io,TH.io))}</div><div class=\"big\">${io.toFixed(2)}<small> MB/s</small></div>\n"
    " <dl class=\"g2\"><div><dt>Read rate</dt><dd>${s.io_r.toFixed(2)} MB/s</dd></div><div><dt>Write rate</dt><dd>${s.io_w.toFixed(2)} MB/s</dd></div><div><dt>Total I/O rate</dt><dd>${io.toFixed(2)} MB/s</dd></div><div><dt>Peak total rate</dt><dd>${s.io_peak.toFixed(2)} MB/s</dd></div>\n"
    " <div><dt>Read operations</dt><dd>${s.io_rops.toFixed(0)} per second</dd></div><div><dt>Write operations</dt><dd>${s.io_wops.toFixed(0)} per second</dd></div></dl>\n"
    " <div class=\"note\">Summed over all physical disks. Warning at ${TH.io[0]} MB/s, critical at ${TH.io[1]} MB/s.</div>`}\n"
    "function renderSel(d,cur){\n"
    " const s=d.sys,ok=cur.r!==null,io=s.io_r+s.io_w;\n"
    " const cp=pv(cur.cpu_h),cc=cv(cur.cpu_h),mp=pv(cur.rss_h),mc=cv(cur.rss_h);\n"
    " const rp=pv(cur.r_h),rc=cv(cur.r_h),wp=pv(cur.w_h),wc=cv(cur.w_h),tp=rp+wp,tc=rc+wc;\n"
    " const cl=lv(cc,TH.cpu),ml=lv(cur.mem,TH.mem),il=ok?lv(tc,TH.io):0;\n"
    " const R=[{m:\"CPU usage\",p:cp,c:cc,tol:2,dec:1,u:\"%\",l:cl},\n"
    "  {m:\"Memory (RSS)\",p:mp,c:mc,tol:.5,dec:1,u:\" MB\",l:ml,x:` (${cur.mem.toFixed(2)}% of RAM)`}];\n"
    " if(ok)R.push({m:\"Disk read\",p:rp,c:rc,tol:.1,dec:2,u:\" MB/s\",l:-1},{m:\"Disk write\",p:wp,c:wc,tol:.1,dec:2,u:\" MB/s\",l:-1},{m:\"Total I/O\",p:tp,c:tc,tol:.1,dec:2,u:\" MB/s\",l:il});\n"
    " R.push({m:\"Threads\",p:cur.pthreads,c:cur.threads,tol:0,dec:0,u:\"\",l:-1});\n"
    " let body=R.map(r=>{const dv=r.c-r.p;return`<tr><td><b>${r.m}</b></td><td class=\"r\">${r.p.toFixed(r.dec)}${r.u}</td><td class=\"r\">${r.c.toFixed(r.dec)}${r.u}${r.x||\"\"}</td><td class=\"r\">${sgn(dv,r.dec,r.u)}</td><td>${AR[trend(dv,r.tol)]}</td><td>${r.l>=0?tagl(r.l):\"-\"}</td></tr>`}).join(\"\");\n"
    " body+=`<tr><td><b>Process state</b></td><td class=\"r\">${SN[cur.pstate]||cur.pstate}</td><td class=\"r\">${SN[cur.state]||cur.state}</td><td class=\"r\">${cur.pstate===cur.state?\"No change\":\"Changed\"}</td><td>-</td><td>-</td></tr>`;\n"
    " if(!ok)body+=`<tr><td><b>Disk I/O</b></td><td colspan=\"5\" class=\"empty\">n/a. Linux hides this process's disk I/O. Start the dashboard with sudo to see it.</td></tr>`;\n"
    " const M=[{k:\"CPU usage\",l:cl,p:cp,c:cc,t:trend(cc-cp,2),f:x=>x.toFixed(1)+\"%\"},{k:\"Memory usage\",l:ml,p:mp,c:mc,t:trend(mc-mp,.5),f:x=>x.toFixed(1)+\" MB\"}];\n"
    " if(ok)M.push({k:\"Disk I/O\",l:il,p:tp,c:tc,t:trend(tc-tp,.1),f:x=>x.toFixed(2)+\" MB/s\"});\n"
    " M.forEach(m=>m.rel=Math.abs(m.c-m.p)/Math.max(m.p,m.c,1e-9));\n"
    " const ph=m=>m.t===\"Increasing\"?`${m.k} increased from ${m.f(m.p)} to ${m.f(m.c)}`:m.t===\"Decreasing\"?`${m.k} decreased from ${m.f(m.p)} to ${m.f(m.c)}`:`${m.k} is stable at ${m.f(m.c)}`;\n"
    " const top=M.slice().sort((a,b)=>b.l-a.l||b.rel-a.rel)[0],worst=top.l;\n"
    " const vd=worst===2?\"is in a critical condition and needs immediate attention\":worst===1?\"requires attention\":top.t===\"Increasing\"?\"is normal but worth watching\":\"is operating normally\";\n"
    " const sc=lv(s.cpu,TH.cpu),sm=lv(s.mem_pct,TH.mem),si=lv(io,TH.io);\n"
    " const ov=Math.max(sc,sm,si,cl,il),why=[];\n"
    " if(sc)why.push(`System CPU is ${s.cpu.toFixed(1)}% (${LV[sc]})`);\n"
    " if(sm)why.push(`System memory is ${s.mem_pct.toFixed(1)}% (${LV[sm]})`);\n"
    " if(si)why.push(`System disk I/O is ${io.toFixed(2)} MB/s (${LV[si]})`);\n"
    " if(cl)why.push(`Process CPU is ${cc.toFixed(1)}% (${LV[cl]})`);\n"
    " if(il)why.push(`Process disk I/O is ${tc.toFixed(2)} MB/s (${LV[il]})`);\n"
    " const share=`${(cc/d.ncpu).toFixed(1)}% of total CPU capacity, ${cur.mem.toFixed(2)}% of RAM`+(ok?` and ${io>0.01?Math.min(100,tc/io*100).toFixed(0)+\"% of disk I/O\":\"no disk I/O right now\"}`:\"\");\n"
    " $(\"#selp\").innerHTML=`<div class=\"mh sh\"><h2>Selected process: ${esc(cur.name)} <span class=\"hs\">PID ${cur.pid}</span>${flagsHtml(cur)}</h2>\n"
    " <span>${sel!==null?'<span class=\"tag ok\">Monitoring every 1 second</span> <button class=\"ghost\" onclick=\"sel=null;tick()\">Stop monitoring</button>':'<span class=\"hs\">Showing the busiest process. Click any process in the table to monitor it.</span>'} <button class=\"end\" onclick=\"openEnd(${cur.pid})\" ${cur.prot?\"disabled\":\"\"}>End process...</button></span></div>\n"
    " ${lost!==null?`<div class=\"warnbox\">PID ${lost} is no longer running, so the dashboard switched to the busiest process.</div>`:\"\"}\n"
    " <div class=\"two\"><div>\n"
    " <div class=\"cond c${worst}\"><div class=\"k\">Process condition</div><p>${ph(top)}, so the process ${vd}.</p><ul>${M.map(m=>`<li><span>${ph(m)}.</span>${tagl(m.l)}</li>`).join(\"\")}${cur.spike?`<li><span>CPU jumped far above this process's own recent average.</span>${tagl(1)}</li>`:\"\"}${cur.leak?`<li><span>Memory has grown steadily over the last 30 seconds, which can mean a leak.</span>${tagl(1)}</li>`:\"\"}</ul></div>\n"
    " <h3>Previous sample vs current sample</h3><div class=\"tw\" style=\"max-height:none\"><table class=\"cmp\"><thead><tr><th>Metric</th><th class=\"r\">Previous</th><th class=\"r\">Current</th><th class=\"r\">Change</th><th>Trend</th><th>Status</th></tr></thead><tbody>${body}</tbody></table></div>\n"
    " <div class=\"note\">Previous is the sample from one second earlier. Trend is Stable when the change is within 2 points for CPU, 0.5 MB for memory and 0.1 MB/s for I/O. Status uses the CPU, memory and I/O limits shown above.</div></div>\n"
    " <div><h3>Process vs system</h3><table class=\"cmp\"><thead><tr><th>Resource</th><th class=\"r\">Process</th><th class=\"r\">System</th><th>Process</th><th>System</th></tr></thead><tbody>\n"
    " <tr><td><b>CPU</b></td><td class=\"r\">${cc.toFixed(1)}%</td><td class=\"r\">${s.cpu.toFixed(1)}%</td><td>${tagl(cl)}</td><td>${tagl(sc)}</td></tr>\n"
    " <tr><td><b>Memory</b></td><td class=\"r\">${mc.toFixed(1)} MB</td><td class=\"r\">${s.mem_pct.toFixed(1)}%</td><td>${tagl(ml)}</td><td>${tagl(sm)}</td></tr>\n"
    " <tr><td><b>Disk I/O</b></td><td class=\"r\">${ok?tc.toFixed(2)+\" MB/s\":\"n/a\"}</td><td class=\"r\">${io.toFixed(2)} MB/s</td><td>${ok?tagl(il):\"-\"}</td><td>${tagl(si)}</td></tr></tbody></table>\n"
    " <div class=\"note\">This process uses ${share}. Process CPU is per core, so 100% is one full core.</div>\n"
    " <div class=\"k\" style=\"margin-top:12px\">Overall health</div><div class=\"badge ${LC[ov]}\">${[\"HEALTHY\",\"ATTENTION\",\"CRITICAL\"][ov]}</div>\n"
    " ${why.length?`<ul>${why.map(x=>`<li>${x}</li>`).join(\"\")}</ul>`:'<div class=\"note\">The process and the whole system are within normal limits.</div>'}</div></div>`}\n"
    "function render(d){\n"
    " $(\"#hc\").textContent=d.count;$(\"#hl\").textContent=d.load.map(x=>x.toFixed(2)).join(\" / \");$(\"#hu\").textContent=dur(d.uptime);\n"
    " TH=d.th||TH;const s=d.sys,[ct2,cc]=st(s.cpu,TH.cpu);\n"
    " $(\"#sc\").innerHTML=s.cpu.toFixed(0)+\"<small>%</small>\";$(\"#sct\").textContent=ct2;$(\"#sct\").className=\"tag \"+cc;\n"
    " $(\"#sm\").innerHTML=s.mem_pct.toFixed(0)+\"<small>%</small>\";$(\"#smd\").textContent=mb(s.mem_used)+\" of \"+mb(s.mem_total)+\", \"+d.ncpu+\" CPU\"+(d.ncpu>1?\"s\":\"\");\n"
    " draw($(\"#chs\"),[{d:s.cpu_h,c:css(\"--cpu\"),fill:1},{d:s.mem_h,c:css(\"--mem\")}],100,lines(TH.cpu));renderRes(d);derive(d);renderNA(d);if(md&&!d.procs.some(x=>x.pid===md.pid))drawMd();\n"
    " const f=$(\"#q\").value.toLowerCase();\n"
    " const rows=d.procs.filter(p=>!f||(p.name+\" \"+p.pid+\" \"+p.user+\" \"+p.cmd).toLowerCase().includes(f));\n"
    " const key=p=>{const v=p[sk];return v===null||v===undefined?-1:v};\n"
    " rows.sort((a,b)=>{const x=key(a),y=key(b);return(typeof x===\"string\"?x.localeCompare(y):x-y)*sd});\n"
    " const cur=d.pinned;if(sel!==null&&(!cur||cur.pid!==sel)){lost=sel;sel=null}\n"
    " $(\"#shown\").textContent=\"Showing \"+rows.length+\" of \"+d.count;\n"
    " $(\"#tb\").innerHTML=rows.slice(0,500).map(p=>{const io=p.r===null?null:p.r+p.w,fl=flagsHtml(p);return`<tr data-p=\"${p.pid}\" class=\"${cur&&p.pid===cur.pid?\"sel\":\"\"}\"><td>${p.pid}</td><td class=\"nm\">${esc(p.name)}</td><td>${esc(p.user)}</td>\n"
    " <td>${p.pstate!==p.state?(SN[p.pstate]||p.pstate)+\" &rarr; \"+(SN[p.state]||p.state):(SN[p.state]||p.state)}</td>\n"
    " <td><span class=\"bar\"><b style=\"width:${Math.min(100,p.cpu)}%\"></b></span>${p.cpu.toFixed(1)}</td><td class=\"r\">${chg(p.dcpu,2,1,\"\")}</td>\n"
    " <td class=\"r\">${p.mem.toFixed(2)}</td><td class=\"r\">${chg(p.dmem,.5,1,\" MB\")}</td>\n"
    " <td class=\"r\">${nv(p.r,x=>x.toFixed(2))}</td><td class=\"r\">${nv(p.w,x=>x.toFixed(2))}</td><td class=\"r\">${p.r===null?\"n/a\":chg(p.dio,.1,2,\"\")}</td>\n"
    " <td>${tagl(p.lvl)}${fl}</td><td class=\"sp\">${spark(p.sp)}</td><td class=\"r\">${mb(p.rss)}</td><td class=\"r\">${mb(p.vsz)}</td>\n"
    " <td class=\"r\">${p.threads}${p.threads!==p.pthreads?\" (\"+sgn(p.threads-p.pthreads,0,\"\")+\")\":\"\"}</td><td class=\"r\">${nv(p.fds,x=>x)}</td><td class=\"cm\">${esc(p.cmd)}</td>\n"
    " <td><button class=\"end\" data-end=\"${p.pid}\" ${p.prot?'disabled title=\"Protected process\"':'title=\"End this process\"'}>End</button></td></tr>`}).join(\"\")||`<tr><td colspan=\"19\" class=\"empty\">No process matches that filter.</td></tr>`;\n"
    " if(cur)renderSel(d,cur);\n"
    " if(cur){const io=(cur.r||0)+(cur.w||0),it=cur.r===null?[\"Not readable\",\"warn\"]:st(io,TH.io);\n"
    "  const tx=tr&&tr.pid===cur.pid?tr.html:\"\";\n"
    "  $(\"#det\").innerHTML=`<h2>${esc(cur.name)} <span class=\"hs\">PID ${cur.pid}</span> <span class=\"hs\">${sel===null?\"(busiest right now)\":\"(selected)\"}</span></h2>\n"
    "  <div class=\"cmd\">${esc(cur.cmd)}</div>\n"
    "  <div class=\"id\"><span>User <b>${esc(cur.user)}</b></span><span>State <b>${SN[cur.state]||cur.state}</b></span><span>Threads <b>${cur.threads}</b></span><span>Open files <b>${nv(cur.fds,x=>x)}</b></span>\n"
    "  <span>Nice <b>${cur.nice}</b></span><span>Running for <b>${dur(cur.age)}</b></span><span>CPU time <b>${ct(cur.cpu_time)}</b></span><span>Context switches <b>${nv(cur.ctx,x=>x.toLocaleString())}</b></span><span>Parent <b>${cur.pname?esc(cur.pname)+\" (PID \"+cur.ppid+\")\":\"PID \"+cur.ppid}</b></span><span>Child processes <b>${cur.nchild}</b></span></div>\n"
    "  ${block(\"CPU\",cur.cpu,\"%\",cur.cpu_h,\"--cpu\",\"dc\",Math.max(100,...cur.cpu_h),TH.cpu)}${block(\"Memory\",cur.mem,\"%\",cur.mem_h,\"--mem\",\"dm\",100,TH.mem)}\n"
    "  <div class=\"blk\"><div class=\"mh\"><span class=\"k\">Memory size</span></div><dl><div><dt>Resident (RSS)</dt><dd>${mb(cur.rss)}</dd></div><div><dt>Virtual</dt><dd>${mb(cur.vsz)}</dd></div></dl></div>\n"
    "  <div class=\"blk\"><div class=\"mh\"><span class=\"k\">Disk I/O</span><span class=\"tag ${it[1]}\">${it[0]}</span></div>\n"
    "  <dl><div><dt>Read MB/s</dt><dd>${nv(cur.r,x=>x.toFixed(2))}</dd></div><div><dt>Write MB/s</dt><dd>${nv(cur.w,x=>x.toFixed(2))}</dd></div><div><dt>Read total</dt><dd>${nv(cur.rt,x=>x.toFixed(1)+\" MB\")}</dd></div><div><dt>Write total</dt><dd>${nv(cur.wt,x=>x.toFixed(1)+\" MB\")}</dd></div></dl></div>\n"
    "  <div class=\"blk\"><div class=\"mh\"><span class=\"k\">System calls</span><button onclick=\"trace(${cur.pid})\">Trace for 3 seconds</button></div>${tx}</div>`;\n"
    "  [[\"dc\",cur.cpu_h],[\"dm\",cur.mem_h]].forEach(([i,h])=>{const e=$(\"#\"+i);draw(e,[{d:h,c:css(e.dataset.c),fill:1}],+e.dataset.m,lines(i===\"dc\"?TH.cpu:TH.mem))})}\n"
    " const io2=s.io_r+s.io_w,sa=[];\n"
    " [[\"System CPU\",s.cpu,\"%\",TH.cpu],[\"System memory\",s.mem_pct,\"%\",TH.mem],[\"System disk I/O\",io2,\" MB/s\",TH.io]].forEach(([nm,v,u,t])=>{const l=lv(v,t);if(l)sa.push(`<li>${tagl(l)}<span><b>${nm}</b> is ${v.toFixed(1)}${u}. Warning at ${t[0]}, critical at ${t[1]}.</span></li>`)});\n"
    " const pa=d.alerts.map(a=>`<li><span class=\"tag crit\">CRITICAL</span><span><b>${esc(a.name)}</b> (PID ${a.pid}) is at ${a.cpu}% CPU, ${a.mem}% memory and ${a.io} MB/s I/O</span></li>`);\n"
    " $(\"#al\").innerHTML=sa.concat(pa).join(\"\")||`<li class=\"empty\">No alerts. The system is within limits and no process has stayed at a critical level for 3 seconds.</li>`;\n"
    " $(\"#ev\").innerHTML=d.events.length?d.events.map(e=>`<li><time>${e.t}</time><span>${esc(e.name)} (PID ${e.pid}) ${e.kind===\"start\"?\"started\":e.kind===\"signal\"?\"received \"+e.note:\"exited\"}</span></li>`).join(\"\"):`<li class=\"empty\">Waiting for processes to start or exit.</li>`}\n"
    "$(\"#na\").onclick=e=>{const m=e.target.closest(\"[data-mon]\"),x=e.target.closest(\"[data-end]\");\n"
    " if(m){lost=null;sel=+m.dataset.mon;tick();$(\"#selp\").scrollIntoView({behavior:\"smooth\",block:\"nearest\"})}else if(x)openEnd(+x.dataset.end)};\n"
    "$(\"#md\").onclick=e=>{if(e.target===$(\"#md\"))closeMd()};\n"
    "document.addEventListener(\"keydown\",e=>{if(e.key===\"Escape\")closeMd()});\n"
    "async function trace(pid){\n"
    " tr={pid,html:'<div class=\"empty\">Tracing for 3 seconds...</div>'};render(last);\n"
    " try{const r=await(await fetch(\"/api/syscalls?pid=\"+pid)).json();\n"
    "  tr={pid,html:r.error?`<div class=\"empty\">${esc(r.error)}</div>`:`<table class=\"tr\"><thead><tr><th>System call</th><th class=\"r\">Calls</th><th class=\"r\">% of time</th></tr></thead><tbody>${r.rows.map(x=>`<tr><td>${esc(x.name)}</td><td class=\"r\">${x.calls}</td><td class=\"r\">${x.pct.toFixed(1)}</td></tr>`).join(\"\")}</tbody></table>`}}\n"
    " catch(e){tr={pid,html:'<div class=\"empty\">Trace failed.</div>'}}\n"
    " render(last)}\n"
    "$(\"#tb\").onclick=e=>{const eb=e.target.closest(\"button[data-end]\");if(eb){openEnd(+eb.dataset.end);return}const r=e.target.closest(\"tr\");if(!r||!r.dataset.p)return;const p=+r.dataset.p;lost=null;sel=sel===p?null:p;tick();$(\"#selp\").scrollIntoView({behavior:\"smooth\",block:\"nearest\"})};\n"
    "$(\"#th\").onclick=e=>{const t=e.target.closest(\"th\");if(!t)return;const k=t.dataset.k;if(k===\"sp\"||k===\"act\")return;sd=sk===k?-sd:([\"name\",\"user\",\"pid\",\"cmd\",\"state\"].includes(k)?1:-1);sk=k;\n"
    " document.querySelectorAll(\"#th th\").forEach(x=>x.classList.toggle(\"on\",x===t));if(last)render(last)};\n"
    "$(\"#q\").oninput=()=>last&&render(last);\n"
    "async function tick(){try{const r=await fetch(\"/api/data\"+(sel!==null?\"?pid=\"+sel:\"\"));last=await r.json();\n"
    " if(!last.procs){return}$(\"#conn\").textContent=\"Live\";$(\"#conn\").className=\"hs on\";render(last)}\n"
    " catch(e){$(\"#conn\").textContent=\"Disconnected. Is the dashboard program still running?\";$(\"#conn\").className=\"hs off\"}}\n"
    "tick();setInterval(tick,1000);\n"
    "</script></body></html>\n"
    "\n"
    "\n"
;

/* ------------------------------------------------------------- utilities */

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* append a value to a history array holding n samples (caller bumps n) */
static void hpush(float *a, int n, float v) {
    if (n >= HIST) {
        memmove(a, a + 1, (HIST - 1) * sizeof(float));
        a[HIST - 1] = v;
    } else {
        a[n] = v;
    }
}

static void add_event(int kind, int pid, const char *name, const char *note) {
    memmove(&events[1], &events[0], (MAXEV - 1) * sizeof(Event));
    time_t tt = time(NULL);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    strftime(events[0].t, sizeof events[0].t, "%H:%M:%S", &tmv);
    events[0].kind = kind;
    events[0].pid = pid;
    snprintf(events[0].name, sizeof events[0].name, "%s", name);
    snprintf(events[0].note, sizeof events[0].note, "%s", note);
    if (nevents < MAXEV) nevents++;
}

static const char *user_of(uid_t uid) {
    static struct { uid_t u; char n[32]; int ok; } cache[256];
    int i;
    for (i = 0; i < 256 && cache[i].ok; i++)
        if (cache[i].u == uid) return cache[i].n;
    if (i == 256) return "?";
    cache[i].ok = 1;
    cache[i].u = uid;
    struct passwd *pw = getpwuid(uid);
    if (pw) snprintf(cache[i].n, sizeof cache[i].n, "%s", pw->pw_name);
    else snprintf(cache[i].n, sizeof cache[i].n, "%u", (unsigned)uid);
    return cache[i].n;
}

/* growable text buffer for building JSON */
typedef struct {
    char *d;
    size_t n, cap;
} Buf;

static int bgrow(Buf *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->n + extra + 1) nc *= 2;
    char *nd = realloc(b->d, nc);
    if (!nd) return 0;
    b->d = nd;
    b->cap = nc;
    return 1;
}

static void bput(Buf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need >= 0 && bgrow(b, (size_t)need)) {
        vsnprintf(b->d + b->n, (size_t)need + 1, fmt, ap2);
        b->n += (size_t)need;
    }
    va_end(ap2);
}

static void bchar(Buf *b, char c) {
    if (bgrow(b, 1)) {
        b->d[b->n++] = c;
        b->d[b->n] = 0;
    }
}

static void bstr(Buf *b, const char *s) {
    bchar(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            bchar(b, '\\');
            bchar(b, (char)c);
        } else if (c < 0x20) {
            bput(b, "\\u%04x", c);
        } else {
            bchar(b, (char)c);
        }
    }
    bchar(b, '"');
}

static void bhist(Buf *b, const float *a, int n, const char *fmt) {
    bchar(b, '[');
    for (int i = 0; i < n; i++) {
        if (i) bchar(b, ',');
        bput(b, fmt, a[i]);
    }
    bchar(b, ']');
}

/* -------------------------------------------------------------- sampling */

static void read_cmdline(int pid, const char *comm, char *out, size_t cap) {
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    size_t n = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
    }
    buf[n] = 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)buf[i] < 0x20) buf[i] = ' ';
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = 0;
    if (n == 0) snprintf(out, cap, "[%s]", comm);
    else snprintf(out, cap, "%.160s", buf);
}

static long read_ctx(int pid) {
    char path[64], line[256];
    long total = 0;
    int found = 0;
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "ctxt_switches:");
        if (p) {
            total += atol(p + 14);
            found = 1;
        }
    }
    fclose(f);
    return found ? total : -1;
}

static int read_fds(int pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/fd", pid);
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

static int read_io(int pid, unsigned long long *rb, unsigned long long *wb) {
    char path[64], line[128];
    int got = 0;
    snprintf(path, sizeof path, "/proc/%d/io", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "read_bytes:", 11)) { *rb = strtoull(line + 11, NULL, 10); got |= 1; }
        else if (!strncmp(line, "write_bytes:", 12)) { *wb = strtoull(line + 12, NULL, 10); got |= 2; }
    }
    fclose(f);
    return got == 3;
}

static Proc *find_proc(int pid) {
    for (int i = 0; i < MAXP; i++)
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
    return NULL;
}

static Proc *new_proc(void) {
    for (int i = 0; i < MAXP; i++)
        if (!procs[i].used) {
            memset(&procs[i], 0, sizeof(Proc));
            procs[i].used = 1;
            return &procs[i];
        }
    return NULL;
}

static int whole_disk(const char *name) {
    static int have_sysblock = -1;
    if (have_sysblock < 0) have_sysblock = access("/sys/block", F_OK) == 0;
    if (!strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) || !strncmp(name, "dm-", 3) ||
        !strncmp(name, "zram", 4) || !strncmp(name, "sr", 2) || !strncmp(name, "fd", 2) ||
        !strncmp(name, "nbd", 3))
        return 0;
    if (have_sysblock) {
        char p[96];
        snprintf(p, sizeof p, "/sys/block/%s", name);
        return access(p, F_OK) == 0;
    }
    return !isdigit((unsigned char)name[strlen(name) - 1]);
}

/* total sectors and operations read/written across all whole disks (/proc/diskstats) */
static void read_disks(unsigned long long *rs, unsigned long long *ws, unsigned long long *ro,
                       unsigned long long *wo) {
    char line[512], dev[64];
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int maj, min;
        unsigned long long rc, rm, sr, rt, wc, wm, sw, wt;
        if (sscanf(line, "%d %d %63s %llu %llu %llu %llu %llu %llu %llu %llu", &maj, &min, dev, &rc, &rm,
                   &sr, &rt, &wc, &wm, &sw, &wt) < 11)
            continue;
        if (!whole_disk(dev)) continue;
        *rs += sr;
        *ws += sw;
        *ro += rc;
        *wo += wc;
    }
    fclose(f);
}

/* Flags unusual behaviour from a process's own history. */
static void analyze(Proc *p) {
    int h = p->hn;
    if (p->spike_ttl > 0) p->spike_ttl--;
    if (h >= 12) { /* CPU spike: far above its own recent average */
        int n = h - 1;
        double sum = 0, sq = 0;
        for (int i = 0; i < n; i++) {
            sum += p->cpu_h[i];
            sq += (double)p->cpu_h[i] * p->cpu_h[i];
        }
        double mean = sum / n, var = sq / n - mean * mean, d = p->cpu_h[h - 1] - mean;
        if (d >= 20.0 && d * d > 9.0 * var) p->spike_ttl = 10;
    }
    p->spike = p->spike_ttl > 0;
    p->leak = 0;
    if (h >= 30) { /* memory keeps growing: most of the last 30 steps went up, 5 MB or more in total */
        int inc = 0;
        for (int i = h - 29; i < h; i++)
            if (p->rss_h[i] > p->rss_h[i - 1] + 0.001f) inc++;
        double growth = p->rss_h[h - 1] - p->rss_h[h - 30];
        if (inc >= 20 && growth >= 5.0) p->leak = 1;
    }
}

/* One sampling pass over /proc. Caller holds the lock. */
static void sample(int first) {
    static unsigned long long prev_total = 0, prev_idle = 0;
    static long page = 0, clk = 0;
    if (!page) { page = sysconf(_SC_PAGESIZE); clk = sysconf(_SC_CLK_TCK); }

    double now = now_s();
    static double prev_now = 0;
    static unsigned long long core_pt[MAXCORES], core_pi[MAXCORES];
    double dt_sys = prev_now > 0 ? now - prev_now : 0.0;
    prev_now = now;

    /* system CPU: overall line plus one line per core */
    char line[512];
    unsigned long long dtotal = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "cpu", 3) != 0) break;
            unsigned long long v[8] = {0};
            int core = -1, off = 3;
            if (isdigit((unsigned char)line[3])) {
                core = atoi(line + 3);
                while (isdigit((unsigned char)line[off])) off++;
            }
            if (sscanf(line + off, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
                       &v[4], &v[5], &v[6], &v[7]) < 4)
                continue;
            unsigned long long total = 0;
            for (int i = 0; i < 8; i++) total += v[i];
            unsigned long long idle = v[3] + v[4];
            if (core < 0) {
                dtotal = prev_total ? total - prev_total : 0;
                unsigned long long di = prev_total ? idle - prev_idle : 0;
                g_cpu = dtotal > 0 ? 100.0 * (1.0 - (double)di / (double)dtotal) : 0.0;
                if (g_cpu < 0) g_cpu = 0;
                prev_total = total;
                prev_idle = idle;
            } else if (core < MAXCORES) {
                unsigned long long dt = core_pt[core] ? total - core_pt[core] : 0;
                unsigned long long di = core_pt[core] ? idle - core_pi[core] : 0;
                core_use[core] = dt > 0 ? 100.0 * (1.0 - (double)di / (double)dt) : 0.0;
                if (core_use[core] < 0) core_use[core] = 0;
                core_pt[core] = total;
                core_pi[core] = idle;
                if (core + 1 > ncores) ncores = core + 1;
            }
        }
        fclose(f);
    }
    if (dtotal > 0) {
        g_cpu_sum += g_cpu;
        g_cpu_cnt++;
        if (g_cpu > g_cpu_peak) g_cpu_peak = g_cpu;
        if (g_cpu < g_cpu_min) g_cpu_min = g_cpu;
    }

    /* memory: total, free, available, plus session average / peak / minimum */
    unsigned long long mt = 0, ma = 0, mfree = 0;
    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "MemTotal:", 9)) sscanf(line + 9, "%llu", &mt);
            else if (!strncmp(line, "MemAvailable:", 13)) sscanf(line + 13, "%llu", &ma);
            else if (!strncmp(line, "MemFree:", 8)) sscanf(line + 8, "%llu", &mfree);
        }
        fclose(f);
    }
    g_mem_total = mt ? mt * 1024ULL : 1;
    g_mem_avail = ma * 1024ULL > g_mem_total ? g_mem_total : ma * 1024ULL;
    g_mem_free = mfree * 1024ULL;
    g_mem_used = g_mem_total - g_mem_avail;
    double mem_pct = 100.0 * (double)g_mem_used / (double)g_mem_total;
    g_mem_sum += mem_pct;
    g_mem_cnt++;
    if (mem_pct > g_mem_peak) g_mem_peak = mem_pct;
    if (mem_pct < g_mem_min) g_mem_min = mem_pct;

    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &g_up) != 1) g_up = 0;
        fclose(f);
    }
    if (getloadavg(g_load, 3) < 0) g_load[0] = g_load[1] = g_load[2] = 0;

    /* disk I/O rate: change in sectors (512 bytes) and operations since last sample */
        /* System disk I/O: calculate rates from /proc/diskstats */
    {
        static unsigned long long prev_read_sectors = 0;
        static unsigned long long prev_write_sectors = 0;
        static unsigned long long prev_reads = 0;
        static unsigned long long prev_writes = 0;
        static int have_previous = 0;

        unsigned long long read_sectors = 0;
        unsigned long long write_sectors = 0;
        unsigned long long reads = 0;
        unsigned long long writes = 0;

        /* Read current disk counters */
        read_disks(
            &read_sectors,
            &write_sectors,
            &reads,
            &writes
        );

        /* Calculate rates only after obtaining the first sample */
        if (have_previous && dt_sys > 0.0) {
            unsigned long long delta_read_sectors =
                read_sectors >= prev_read_sectors
                    ? read_sectors - prev_read_sectors : 0;

            unsigned long long delta_write_sectors =
                write_sectors >= prev_write_sectors
                    ? write_sectors - prev_write_sectors : 0;

            unsigned long long delta_reads =
                reads >= prev_reads ? reads - prev_reads : 0;

            unsigned long long delta_writes =
                writes >= prev_writes ? writes - prev_writes : 0;

            /* Linux disk sectors are 512 bytes each */
            g_dr = (double)delta_read_sectors
                   * 512.0 / 1048576.0 / dt_sys;

            g_dw = (double)delta_write_sectors
                   * 512.0 / 1048576.0 / dt_sys;

            /* Read/write operations per second */
            g_drops = (double)delta_reads / dt_sys;
            g_dwops = (double)delta_writes / dt_sys;

            /* Record the highest combined read/write rate */
            if (g_dr + g_dw > g_io_peak) {
                g_io_peak = g_dr + g_dw;
            }
        } else {
            /* No previous sample is available yet */
            g_dr = 0.0;
            g_dw = 0.0;
            g_drops = 0.0;
            g_dwops = 0.0;
        }

        /* Save counters for the next sample */
        prev_read_sectors = read_sectors;
        prev_write_sectors = write_sectors;
        prev_reads = reads;
        prev_writes = writes;
        have_previous = 1;

    }
    hpush(sys_cpu_h, sys_n, (float)g_cpu);
    hpush(sys_mem_h, sys_n, (float)mem_pct);
    hpush(sys_io_r_h, sys_n, (float)g_dr);
    hpush(sys_io_w_h, sys_n, (float)g_dw);
    if (sys_n < HIST) sys_n++;

    /* processes */
    for (int i = 0; i < MAXP; i++) procs[i].seen = 0;
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        int pid = atoi(e->d_name);
        if (pid <= 0) continue;

        char path[64], buf[2048];
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        FILE *sf = fopen(path, "r");
        if (!sf) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, sf);
        fclose(sf);
        buf[n] = 0;
        char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
        if (!lp || !rp || rp < lp) continue;
        char comm[64];
        size_t cl = (size_t)(rp - lp - 1);
        if (cl > 63) cl = 63;
        memcpy(comm, lp + 1, cl);
        comm[cl] = 0;
        char st;
        unsigned long ut, stt;
        long nice, thr;
        int ppid;
        unsigned long long start, vsz;
        if (sscanf(rp + 2,
                   "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %ld %ld %*d %llu %llu",
                   &st, &ppid, &ut, &stt, &nice, &thr, &start, &vsz) != 8)
            continue;
        unsigned long long pages = 0;
        snprintf(path, sizeof path, "/proc/%d/statm", pid);
        FILE *mf = fopen(path, "r");
        if (!mf) continue;
        if (fscanf(mf, "%*u %llu", &pages) != 1) pages = 0;
        fclose(mf);

        unsigned long long jif = (unsigned long long)ut + stt;
        int isnew = 0;
        Proc *p = find_proc(pid);
        if (!p) {
            isnew = 1;
            p = new_proc();
            if (!p) continue;
            p->pid = pid;
            p->prev_j = jif;
            snprintf(p->name, sizeof p->name, "%s", comm);
            struct stat sb;
            snprintf(path, sizeof path, "/proc/%d", pid);
            snprintf(p->user, sizeof p->user, "%s", stat(path, &sb) == 0 ? user_of(sb.st_uid) : "?");
            read_cmdline(pid, comm, p->cmd, sizeof p->cmd);
            if (!first) add_event(1, pid, comm, "");
        } else if (dtotal > 0) {
            double c = (double)(jif - p->prev_j) / (double)dtotal * 100.0 * g_ncpu;
            p->cpu = c < 0 ? 0 : c;
        }
        p->seen = 1;
        p->ppid = ppid;
        p->start = start;
        p->prev_j = jif;
        if (isnew) {
            p->prev_state = st;
            p->prev_threads = (int)thr;
        } else {
            p->prev_state = p->state;
            p->prev_threads = p->threads;
        }
        p->state = st;
        p->threads = (int)thr;
        p->nice = (int)nice;
        p->vsz = vsz;
        p->rss = pages * (unsigned long long)page;
        p->mem = (double)p->rss / (double)g_mem_total * 100.0;
        p->cpu_time = (double)jif / (double)clk;
        p->age = g_up - (double)start / (double)clk;
        if (p->age < 0) p->age = 0;
        p->fds = read_fds(pid);
        p->ctx = read_ctx(pid);
        snprintf(p->name, sizeof p->name, "%s", comm);

        unsigned long long rb = 0, wb = 0;
        if (read_io(pid, &rb, &wb)) {
            if (p->io_ok && now > p->io_t) {
                double dt = now - p->io_t;
                p->r = (rb > p->io_r ? (double)(rb - p->io_r) : 0.0) / dt / 1048576.0;
                p->w = (wb > p->io_w ? (double)(wb - p->io_w) : 0.0) / dt / 1048576.0;
            } else {
                p->r = p->w = 0.0;
            }
            p->io_r = rb;
            p->io_w = wb;
            p->io_t = now;
            p->rt = (double)rb / 1048576.0;
            p->wt = (double)wb / 1048576.0;
            p->io_ok = 1;
        } else {
            p->io_ok = 0;
        }

        hpush(p->cpu_h, p->hn, (float)p->cpu);
        hpush(p->mem_h, p->hn, (float)p->mem);
        hpush(p->r_h, p->hn, p->io_ok ? (float)p->r : 0.0f);
        hpush(p->w_h, p->hn, p->io_ok ? (float)p->w : 0.0f);
        hpush(p->rss_h, p->hn, (float)((double)p->rss / 1048576.0));
        if (p->hn < HIST) p->hn++;
        analyze(p);
        p->hi = (p->cpu >= CPU_CRITICAL || p->mem >= MEMORY_CRITICAL ||
                 (p->io_ok && p->r + p->w >= IO_CRITICAL)) ? p->hi + 1 : 0;
    }
    closedir(d);

    for (int i = 0; i < MAXP; i++)
        if (procs[i].used && !procs[i].seen) {
            add_event(0, procs[i].pid, procs[i].name, "");
            procs[i].used = 0;
        }
    /* count each process's children */
    for (int i = 0; i < MAXP; i++) procs[i].nchild = 0;
    for (int i = 0; i < MAXP; i++)
        if (procs[i].used && procs[i].ppid > 0) {
            Proc *par = find_proc(procs[i].ppid);
            if (par) par->nchild++;
        }
}

static void *sampler(void *arg) {
    (void)arg;
    for (;;) {
        double t0 = now_s();
        pthread_mutex_lock(&lk);
        sample(0);
        pthread_mutex_unlock(&lk);
        double el = now_s() - t0;
        if (el < 1.0) usleep((useconds_t)((1.0 - el) * 1e6));
    }
    return NULL;
}

/* ------------------------------------------------------------------ JSON */

static void proc_json(Buf *b, const Proc *p, int hist) {
    bput(b, "{\"pid\":%d,\"name\":", p->pid);
    bstr(b, p->name);
    bput(b, ",\"user\":");
    bstr(b, p->user);
    bput(b, ",\"state\":\"%c\",\"threads\":%d,\"cpu\":%.2f,\"mem\":%.3f,\"rss\":%llu,\"vsz\":%llu,"
            "\"fds\":%d,\"nice\":%d,\"cpu_time\":%.2f,\"age\":%.1f,",
         p->state, p->threads, p->cpu, p->mem, p->rss, p->vsz, p->fds, p->nice, p->cpu_time, p->age);
    bput(b, "\"pstate\":\"%c\",\"pthreads\":%d,", p->prev_state ? p->prev_state : p->state, p->prev_threads);
    if (p->ctx >= 0) bput(b, "\"ctx\":%ld,", p->ctx);
    else bput(b, "\"ctx\":null,");
    if (p->io_ok) bput(b, "\"r\":%.3f,\"w\":%.3f,\"rt\":%.2f,\"wt\":%.2f,", p->r, p->w, p->rt, p->wt);
    else bput(b, "\"r\":null,\"w\":null,\"rt\":null,\"wt\":null,");
    {
        int h = p->hn;
        double pc = h >= 2 ? p->cpu_h[h - 2] : p->cpu;
        double pr = h >= 2 ? p->rss_h[h - 2] : (double)p->rss / 1048576.0;
        double pi = h >= 2 ? p->r_h[h - 2] + p->w_h[h - 2] : 0.0;
        int prot = p->pid <= 1 || p->pid == self_pid || p->pid == parent_pid || p->ppid == 2 || p->pid == 2;
        bput(b, "\"ppid\":%d,\"nchild\":%d,\"start\":%llu,\"prot\":%d,\"spike\":%d,\"leak\":%d,"
                "\"pcpu\":%.2f,\"prss\":%.2f,\"pio\":%.3f,\"sp\":[",
             p->ppid, p->nchild, p->start, prot, p->spike, p->leak, pc, pr, pi);
        int from = h > 24 ? h - 24 : 0;
        for (int i = from; i < h; i++) bput(b, "%s%.0f", i > from ? "," : "", p->cpu_h[i]);
        bput(b, "],");
    }
    bput(b, "\"cmd\":");
    bstr(b, p->cmd);
    if (hist) {
        bput(b, ",\"cpu_h\":");
        bhist(b, p->cpu_h, p->hn, "%.1f");
        bput(b, ",\"mem_h\":");
        bhist(b, p->mem_h, p->hn, "%.2f");
        bput(b, ",\"r_h\":");
        bhist(b, p->r_h, p->hn, "%.3f");
        bput(b, ",\"w_h\":");
        bhist(b, p->w_h, p->hn, "%.3f");
        bput(b, ",\"rss_h\":");
        bhist(b, p->rss_h, p->hn, "%.2f");
        bput(b, ",\"pname\":");
        Proc *par = find_proc(p->ppid);
        if (par) bstr(b, par->name);
        else bput(b, "null");
    }
    bchar(b, '}');
}

static void data_json(Buf *b, int want_pid) {
    pthread_mutex_lock(&lk);
    int count = 0, top = -1;
    double topcpu = -1;
    for (int i = 0; i < MAXP; i++)
        if (procs[i].used) {
            count++;
            if (procs[i].cpu > topcpu) { topcpu = procs[i].cpu; top = procs[i].pid; }
        }
    bput(b, "{\"ncpu\":%d,\"count\":%d,\"uptime\":%.1f,\"load\":[%.2f,%.2f,%.2f],", g_ncpu, count, g_up,
         g_load[0], g_load[1], g_load[2]);
    bput(b, "\"th\":{\"cpu\":[%.0f,%.0f],\"mem\":[%.0f,%.0f],\"io\":[%.0f,%.0f]},", CPU_WARNING,
         CPU_CRITICAL, MEMORY_WARNING, MEMORY_CRITICAL, IO_WARNING, IO_CRITICAL);
    double mp = 100.0 * (double)g_mem_used / (double)g_mem_total;
    bput(b, "\"sys\":{\"cpu\":%.1f,\"cpu_avg\":%.1f,\"cpu_peak\":%.1f,\"cpu_min\":%.1f,", g_cpu,
         g_cpu_cnt ? g_cpu_sum / (double)g_cpu_cnt : 0.0, g_cpu_peak, g_cpu_cnt ? g_cpu_min : 0.0);
    bput(b, "\"mem_pct\":%.1f,\"mem_used\":%llu,\"mem_total\":%llu,\"mem_free\":%llu,\"mem_avail\":%llu,"
            "\"mem_avg\":%.1f,\"mem_peak\":%.1f,\"mem_min\":%.1f,",
         mp, g_mem_used, g_mem_total, g_mem_free, g_mem_avail, g_mem_cnt ? g_mem_sum / (double)g_mem_cnt : mp,
         g_mem_peak, g_mem_cnt ? g_mem_min : mp);
    bput(b, "\"io_r\":%.3f,\"io_w\":%.3f,\"io_rops\":%.1f,\"io_wops\":%.1f,\"io_peak\":%.3f,\"cores\":[", g_dr,
         g_dw, g_drops, g_dwops, g_io_peak);
    for (int i = 0; i < ncores; i++) bput(b, "%s%.1f", i ? "," : "", core_use[i]);
    bput(b, "],\"cpu_h\":");
    bhist(b, sys_cpu_h, sys_n, "%.1f");
    bput(b, ",\"mem_h\":");
    bhist(b, sys_mem_h, sys_n, "%.1f");
    bput(b, ",\"io_r_h\":");
    bhist(b, sys_io_r_h, sys_n, "%.3f");
    bput(b, ",\"io_w_h\":");
    bhist(b, sys_io_w_h, sys_n, "%.3f");
    bput(b, "},\"top_pid\":%d,\"procs\":[", top);
    int firstp = 1;
    for (int i = 0; i < MAXP; i++)
        if (procs[i].used) {
            if (!firstp) bchar(b, ',');
            firstp = 0;
            proc_json(b, &procs[i], 0);
        }
    bput(b, "],\"alerts\":[");
    int firsta = 1;
    for (int i = 0; i < MAXP; i++)
        if (procs[i].used && procs[i].hi >= NEED) {
            if (!firsta) bchar(b, ',');
            firsta = 0;
            bput(b, "{\"pid\":%d,\"name\":", procs[i].pid);
            bstr(b, procs[i].name);
            bput(b, ",\"cpu\":%.1f,\"mem\":%.1f,\"io\":%.2f}", procs[i].cpu, procs[i].mem,
                 procs[i].io_ok ? procs[i].r + procs[i].w : 0.0);
        }
    bput(b, "],\"events\":[");
    for (int i = 0; i < nevents && i < 12; i++) {
        if (i) bchar(b, ',');
        bput(b, "{\"t\":\"%s\",\"kind\":\"%s\",\"note\":\"%s\",\"pid\":%d,\"name\":", events[i].t,
             events[i].kind == 1 ? "start" : events[i].kind == 2 ? "signal" : "exit", events[i].note,
             events[i].pid);
        bstr(b, events[i].name);
        bchar(b, '}');
    }
    bchar(b, ']');
    Proc *pin = want_pid > 0 ? find_proc(want_pid) : NULL;
    if (!pin) pin = find_proc(top);
    if (pin) {
        bput(b, ",\"pinned\":");
        proc_json(b, pin, 1);
    }
    bchar(b, '}');
    pthread_mutex_unlock(&lk);
}

/* ------------------------------------------------------- syscall tracing */

typedef struct {
    char name[48];
    long calls;
    double pct;
} Row;

static int cmp_row(const void *a, const void *b) {
    long x = ((const Row *)a)->calls, y = ((const Row *)b)->calls;
    return (y > x) - (y < x);
}

static int all_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static void trace_json(Buf *b, int pid) {
    char cmd[128], line[512];
    static Row rows[256];
    int nr = 0, notperm = 0, gone = 0, missing = 0;
    snprintf(cmd, sizeof cmd, "timeout -s INT 3 strace -c -p %d 2>&1", pid);
    FILE *f = popen(cmd, "r");
    if (!f) {
        bput(b, "{\"error\":\"Could not start strace.\"}");
        return;
    }
    while (fgets(line, sizeof line, f)) {
        if (strcasestr(line, "not permitted") || strcasestr(line, "permission denied")) notperm = 1;
        if (strcasestr(line, "no such process")) gone = 1;
        if (strcasestr(line, "not found") || strcasestr(line, "no such file")) missing = 1;
        char *tok[8];
        int nt = 0;
        for (char *t = strtok(line, " \t\r\n"); t && nt < 8; t = strtok(NULL, " \t\r\n")) tok[nt++] = t;
        if (nt >= 5 && isdigit((unsigned char)tok[0][0]) && strcmp(tok[nt - 1], "total") != 0 &&
            all_digits(tok[3]) && nr < 256) {
            snprintf(rows[nr].name, sizeof rows[nr].name, "%s", tok[nt - 1]);
            rows[nr].calls = atol(tok[3]);
            rows[nr].pct = atof(tok[0]);
            nr++;
        }
    }
    pclose(f);
    if (nr > 0) {
        qsort(rows, (size_t)nr, sizeof(Row), cmp_row);
        bput(b, "{\"rows\":[");
        for (int i = 0; i < nr && i < 12; i++) {
            if (i) bchar(b, ',');
            bput(b, "{\"name\":");
            bstr(b, rows[i].name);
            bput(b, ",\"calls\":%ld,\"pct\":%.1f}", rows[i].calls, rows[i].pct);
        }
        bput(b, "]}");
    } else if (missing) {
        bput(b, "{\"error\":\"strace is not installed. Run: sudo apt install strace\"}");
    } else if (notperm) {
        bput(b, "{\"error\":\"Linux blocked tracing. Stop the dashboard and start it with: sudo ./dashboard\"}");
    } else if (gone) {
        bput(b, "{\"error\":\"That process has already exited.\"}");
    } else {
        bput(b, "{\"error\":\"No system calls in 3 seconds. The process is idle or waiting.\"}");
    }
}

/* ----------------------------------------------------- process control */

/* state, parent and start time of a process; 0 if it no longer exists */
static int read_stat3(int pid, char *state, int *ppid, unsigned long long *start) {
    char path[64], buf[2048];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *rp = strrchr(buf, ')');
    if (!rp) return 0;
    return sscanf(rp + 2, "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                  state, ppid, start) == 3;
}

static void sig_fail(Buf *b, const char *msg) {
    bput(b, "{\"ok\":false,\"message\":\"%s\"}", msg);
}

/* Sends TERM, KILL, STOP or CONT to one process after safety checks. */
static void signal_json(Buf *b, int pid, unsigned long long start, const char *sg) {
    int signo;
    if (!strcmp(sg, "TERM")) signo = SIGTERM;
    else if (!strcmp(sg, "KILL")) signo = SIGKILL;
    else if (!strcmp(sg, "STOP")) signo = SIGSTOP;
    else if (!strcmp(sg, "CONT")) signo = SIGCONT;
    else { sig_fail(b, "Unknown signal."); return; }
    if (pid <= 1) { sig_fail(b, "Protected: PID 1 is the init process. Ending it would crash the system."); return; }
    if (pid == self_pid) { sig_fail(b, "Protected: this is the dashboard itself. Stop it with Ctrl+C in the terminal."); return; }
    if (pid == parent_pid) { sig_fail(b, "Protected: this is the shell that started the dashboard."); return; }
    char st = 0;
    int ppid = 0;
    unsigned long long cur = 0;
    if (!read_stat3(pid, &st, &ppid, &cur)) { sig_fail(b, "That process has already exited."); return; }
    if (cur != start) { sig_fail(b, "That PID now belongs to a different process. Nothing was sent."); return; }
    if (pid == 2 || ppid == 2) { sig_fail(b, "Protected: this is a kernel thread."); return; }
    if (st == 'Z') { sig_fail(b, "This is a zombie. It is already finished and waits for its parent to collect it."); return; }
    if (kill(pid, signo) != 0) {
        if (errno == EPERM) sig_fail(b, "Permission denied. Start the dashboard with sudo to control other users' processes.");
        else if (errno == ESRCH) sig_fail(b, "That process has already exited.");
        else sig_fail(b, "The kernel refused the signal.");
        return;
    }
    pthread_mutex_lock(&lk);
    Proc *p = find_proc(pid);
    add_event(2, pid, p ? p->name : "?", sg[0] == 'T' ? "SIGTERM" : sg[0] == 'K' ? "SIGKILL" : sg[1] == 'T' ? "SIGSTOP" : "SIGCONT");
    pthread_mutex_unlock(&lk);
    if (signo == SIGTERM || signo == SIGKILL) {
        int gone = 0;
        for (int i = 0; i < 20 && !gone; i++) {
            usleep(100000);
            char s2 = 0;
            int pp2 = 0;
            unsigned long long st2 = 0;
            if (!read_stat3(pid, &s2, &pp2, &st2) || st2 != start || s2 == 'Z') gone = 1;
        }
        if (gone) bput(b, "{\"ok\":true,\"exited\":1,\"message\":\"The process ended (%s).\"}", signo == SIGTERM ? "SIGTERM" : "SIGKILL");
        else if (signo == SIGTERM) bput(b, "{\"ok\":true,\"exited\":0,\"message\":\"SIGTERM was sent but the process is still running after 2 seconds. You can force kill it.\"}");
        else bput(b, "{\"ok\":true,\"exited\":0,\"message\":\"SIGKILL was sent. The process is still being removed by the kernel.\"}");
    } else if (signo == SIGSTOP) {
        bput(b, "{\"ok\":true,\"exited\":0,\"message\":\"The process is paused (SIGSTOP). Use Resume to continue it.\"}");
    } else {
        bput(b, "{\"ok\":true,\"exited\":0,\"message\":\"The process was resumed (SIGCONT).\"}");
    }
}

/* ------------------------------------------------------------------ HTTP */

static void send_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

static void respond(int fd, int code, const char *ctype, const char *body, size_t len) {
    char head[400];
    const char *why = code == 200 ? "OK" : code == 403 ? "Forbidden" : code == 405 ? "Method Not Allowed" : "Not Found";
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Cache-Control: no-store\r\nX-Frame-Options: DENY\r\n"
                      "Content-Security-Policy: frame-ancestors 'none'\r\nX-Content-Type-Options: nosniff\r\n"
                      "Connection: close\r\n\r\n",
                      code, why, ctype, len);
    send_all(fd, head, (size_t)hl);
    send_all(fd, body, len);
}

static int header_val(const char *req, const char *name, char *out, size_t cap) {
    char key[48];
    snprintf(key, sizeof key, "\r\n%s:", name);
    const char *p = strcasestr(req, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i + 1 < cap) out[i++] = *p++;
    while (i > 0 && out[i - 1] == ' ') i--;
    out[i] = 0;
    return 1;
}

/* Only accept requests addressed to localhost, which blocks DNS-rebinding attacks. */
static int host_ok(const char *req) {
    char h[128], a[64], b[64];
    snprintf(a, sizeof a, "localhost:%d", g_port);
    snprintf(b, sizeof b, "127.0.0.1:%d", g_port);
    if (!header_val(req, "Host", h, sizeof h)) return 0;
    return !strcasecmp(h, a) || !strcasecmp(h, b);
}

static int origin_ok(const char *req) {
    char o[128], a[80], b[80];
    if (!header_val(req, "Origin", o, sizeof o)) return 1;
    snprintf(a, sizeof a, "http://localhost:%d", g_port);
    snprintf(b, sizeof b, "http://127.0.0.1:%d", g_port);
    return !strcasecmp(o, a) || !strcasecmp(o, b);
}

static void *client(void *arg) {
    int fd = (int)(intptr_t)arg;
    char req[4096], method[8] = "", full[1024] = "", tok[80];
    ssize_t n = recv(fd, req, sizeof req - 1, 0);
    if (n <= 0 || sscanf((req[n] = 0, req), "%7s %1023s", method, full) != 2) {
        close(fd);
        return NULL;
    }
    int is_get = !strcmp(method, "GET"), is_post = !strcmp(method, "POST");
    if (!is_get && !is_post) {
        respond(fd, 405, "text/plain", "method not allowed", 18);
        close(fd);
        return NULL;
    }
    if (!host_ok(req)) {
        respond(fd, 403, "text/plain", "forbidden host", 14);
        close(fd);
        return NULL;
    }
    int pid = -1;
    unsigned long long start = 0;
    char sg[8] = "";
    char *q = strchr(full, '?');
    if (q) {
        *q++ = 0;
        char *pp = strstr(q, "pid=");
        if (pp) pid = atoi(pp + 4);
        pp = strstr(q, "start=");
        if (pp) start = strtoull(pp + 6, NULL, 10);
        pp = strstr(q, "sig=");
        if (pp) {
            size_t i = 0;
            for (pp += 4; isalpha((unsigned char)*pp) && i < sizeof sg - 1; pp++) sg[i++] = *pp;
            sg[i] = 0;
        }
    }
    if (is_get && !strcmp(full, "/")) {
        respond(fd, 200, "text/html; charset=utf-8", g_page, g_page_len);
    } else if (is_get && !strcmp(full, "/api/data")) {
        Buf b = {0};
        data_json(&b, pid);
        respond(fd, 200, "application/json", b.d ? b.d : "{}", b.d ? b.n : 2);
        free(b.d);
    } else if (is_get && !strcmp(full, "/api/syscalls")) {
        Buf b = {0};
        if (pid > 0) trace_json(&b, pid);
        else bput(&b, "{\"error\":\"Missing PID.\"}");
        respond(fd, 200, "application/json", b.d ? b.d : "{}", b.d ? b.n : 2);
        free(b.d);
    } else if (is_post && !strcmp(full, "/api/signal")) {
        /* needs the secret token from our own page, so other websites cannot trigger it */
        if (!origin_ok(req) || !header_val(req, "X-Token", tok, sizeof tok) || strcmp(tok, g_token) != 0) {
            respond(fd, 403, "application/json", "{\"ok\":false,\"message\":\"Request refused: missing or wrong token.\"}", 62);
        } else {
            Buf b = {0};
            if (pid > 0) signal_json(&b, pid, start, sg);
            else sig_fail(&b, "Missing PID.");
            respond(fd, 200, "application/json", b.d ? b.d : "{}", b.d ? b.n : 2);
            free(b.d);
        }
    } else {
        respond(fd, 404, "text/plain", "not found", 9);
    }
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ main */

static void on_sigint(int sig) {
    (void)sig;
    const char msg[] = "\nStopped.\n";
    ssize_t r = write(1, msg, sizeof msg - 1);
    (void)r;
    _exit(0);
}

static void open_browser(const char *url) {
    char cmd[256], ver[512] = "";
    int wsl = 0;
    FILE *f = fopen("/proc/version", "r");
    if (f) {
        if (fgets(ver, sizeof ver, f))
            for (char *p = ver; *p; p++) *p = (char)tolower((unsigned char)*p);
        wsl = strstr(ver, "microsoft") != NULL;
        fclose(f);
    }
    snprintf(cmd, sizeof cmd, "%s %s >/dev/null 2>&1 &", wsl ? "explorer.exe" : "xdg-open", url);
    int rc = system(cmd);
    (void)rc;
}

int main(int argc, char **argv) {
    int port = 8000, auto_open = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-browser")) auto_open = 0;
        else {
            printf("Usage: %s [--port N] [--no-browser]\n", argv[0]);
            return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 1;
        }
    }
    g_port = port;
    self_pid = (int)getpid();
    parent_pid = (int)getppid();
    {   /* random per-run token that only our own page knows */
        unsigned char rnd[16] = {0};
        FILE *ur = fopen("/dev/urandom", "rb");
        if (ur) {
            if (fread(rnd, 1, sizeof rnd, ur) != sizeof rnd) memset(rnd, 0, sizeof rnd);
            fclose(ur);
        }
        for (int i = 0; i < 16; i++) snprintf(g_token + 2 * i, 3, "%02x", rnd[i]);
        const char *mark = strstr(HTML, "__TOKEN__");
        size_t pre = mark ? (size_t)(mark - HTML) : sizeof HTML - 1;
        g_page = malloc(sizeof HTML + 64);
        if (!g_page) return 1;
        memcpy(g_page, HTML, pre);
        size_t off = pre;
        if (mark) {
            memcpy(g_page + off, g_token, 32);
            off += 32;
            size_t rest = sizeof HTML - 1 - pre - 9;
            memcpy(g_page + off, mark + 9, rest);
            off += rest;
        }
        g_page_len = off;
    }
    g_ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_ncpu < 1) g_ncpu = 1;

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    pthread_mutex_lock(&lk);
    sample(1);
    pthread_mutex_unlock(&lk);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(srv, 64) < 0) {
        fprintf(stderr, "Cannot listen on port %d: %s\nTry: %s --port 8080\n", port, strerror(errno), argv[0]);
        return 1;
    }

    pthread_t th;
    pthread_create(&th, NULL, sampler, NULL);
    pthread_detach(th);

    char url[64];
    snprintf(url, sizeof url, "http://localhost:%d", port);
    printf("Kernel Monitor dashboard: %s  (Ctrl+C to stop)\n", url);
    fflush(stdout);
    if (auto_open) open_browser(url);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        pthread_t ct;
        if (pthread_create(&ct, NULL, client, (void *)(intptr_t)fd) == 0) pthread_detach(ct);
        else close(fd);
    }
}