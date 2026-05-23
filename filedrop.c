/*
 * filedrop.c  –  bidirectional local Wi-Fi file transfer (Windows, GCC)
 *
 * Compile:
 *   gcc -o filedrop.exe filedrop.c -lws2_32 -O2 -Wall -std=c99
 *
 * Usage:
 *   filedrop.exe                    serves current directory on port 8000
 *   filedrop.exe C:\path\to\folder  serves that folder
 *   filedrop.exe --port 9000        custom port
 *   filedrop.exe C:\folder --port 9000
 */

#define __USE_MINGW_ANSI_STDIO 1   /* proper %lld etc. in MinGW */
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>

/* link with ws2_32 when using MSVC; with GCC use: -lws2_32 */
#ifdef _MSC_VER
#  pragma comment(lib, "ws2_32.lib")
#endif

/* ── tunables ─────────────────────────────────────────────────────────── */
#define PORT_DEFAULT  8000
#define HDR_CAP       16384   /* max HTTP header bytes we buffer            */
#define IO_CAP        65536   /* socket read chunk                          */

static char g_dir[MAX_PATH];
static int  g_port = PORT_DEFAULT;

/* ── HTML page ────────────────────────────────────────────────────────── */
/* JS uses single-quoted strings to keep C escaping manageable.
   Template literals are rewritten as concatenation.                        */
static const char HTML[] =
"<!DOCTYPE html>\n"
"<html lang='en'>\n"
"<head>\n"
"<meta charset='UTF-8'>\n"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>\n"
"<title>FileDrop</title>\n"
"<link rel='preconnect' href='https://fonts.googleapis.com'>\n"
"<link href='https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=Syne:wght@400;600;800&display=swap' rel='stylesheet'>\n"
"<style>\n"
"  :root {\n"
"    --bg:#0a0a0f; --surface:#12121a; --surface2:#1a1a26; --border:#2a2a40;\n"
"    --accent:#7c6aff; --accent2:#ff6a9e; --text:#e8e8f0; --muted:#666680;\n"
"    --success:#4ade80; --danger:#f87171;\n"
"    --mono:'Space Mono',monospace; --sans:'Syne',sans-serif;\n"
"  }\n"
"  *{box-sizing:border-box;margin:0;padding:0}\n"
"  body{background:var(--bg);color:var(--text);font-family:var(--sans);\n"
"    min-height:100vh;padding:24px 16px 80px;\n"
"    background-image:\n"
"      radial-gradient(ellipse 60% 40% at 20% 10%,rgba(124,106,255,.08) 0%,transparent 60%),\n"
"      radial-gradient(ellipse 40% 30% at 80% 80%,rgba(255,106,158,.06) 0%,transparent 60%);}\n"
"  header{text-align:center;margin-bottom:40px;padding-top:16px}\n"
"  .logo{font-size:2.4rem;font-weight:800;letter-spacing:-1px;\n"
"    background:linear-gradient(135deg,var(--accent),var(--accent2));\n"
"    -webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text}\n"
"  .tagline{font-family:var(--mono);font-size:.72rem;color:var(--muted);\n"
"    letter-spacing:2px;text-transform:uppercase;margin-top:4px}\n"
"  .grid{display:grid;grid-template-columns:1fr 1fr;gap:20px;max-width:900px;margin:0 auto}\n"
"  @media(max-width:640px){.grid{grid-template-columns:1fr}}\n"
"  .panel{background:var(--surface);border:1px solid var(--border);border-radius:16px;\n"
"    padding:24px;position:relative;overflow:hidden}\n"
"  .panel::before{content:'';position:absolute;top:0;left:0;right:0;height:2px;border-radius:16px 16px 0 0}\n"
"  .panel.upload::before{background:linear-gradient(90deg,var(--accent),var(--accent2))}\n"
"  .panel.download::before{background:linear-gradient(90deg,var(--accent2),var(--accent))}\n"
"  .panel-title{font-size:.7rem;font-family:var(--mono);letter-spacing:3px;\n"
"    text-transform:uppercase;color:var(--muted);margin-bottom:16px}\n"
"  .dropzone{border:2px dashed var(--border);border-radius:12px;padding:32px 20px;\n"
"    text-align:center;cursor:pointer;transition:all .2s;background:var(--surface2);position:relative}\n"
"  .dropzone:hover,.dropzone.drag-over{border-color:var(--accent);background:rgba(124,106,255,.07)}\n"
"  .dropzone input[type=file]{position:absolute;inset:0;opacity:0;cursor:pointer;width:100%;height:100%}\n"
"  .drop-icon{font-size:2.2rem;margin-bottom:10px;display:block}\n"
"  .drop-label{font-size:.9rem;font-weight:600;color:var(--text);margin-bottom:4px}\n"
"  .drop-sub{font-family:var(--mono);font-size:.68rem;color:var(--muted)}\n"
"  .file-list{margin-top:16px;display:flex;flex-direction:column;gap:8px;max-height:320px;overflow-y:auto}\n"
"  .file-list::-webkit-scrollbar{width:4px}\n"
"  .file-list::-webkit-scrollbar-track{background:transparent}\n"
"  .file-list::-webkit-scrollbar-thumb{background:var(--border);border-radius:4px}\n"
"  .file-item{display:flex;align-items:center;gap:10px;padding:10px 12px;\n"
"    background:var(--surface2);border:1px solid var(--border);border-radius:10px;transition:border-color .2s}\n"
"  .file-item:hover{border-color:var(--accent)}\n"
"  .file-icon{font-size:1.1rem;flex-shrink:0}\n"
"  .file-info{flex:1;min-width:0}\n"
"  .file-name{font-size:.82rem;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
"  .file-size{font-family:var(--mono);font-size:.65rem;color:var(--muted);margin-top:2px}\n"
"  .btn-download{background:rgba(124,106,255,.15);border:1px solid rgba(124,106,255,.3);\n"
"    color:var(--accent);border-radius:8px;padding:6px 12px;font-family:var(--mono);\n"
"    font-size:.7rem;cursor:pointer;text-decoration:none;transition:all .15s;\n"
"    white-space:nowrap;flex-shrink:0}\n"
"  .btn-download:hover{background:rgba(124,106,255,.3);border-color:var(--accent)}\n"
"  .progress-container{margin-top:12px;display:flex;flex-direction:column;gap:8px}\n"
"  .progress-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:4px}\n"
"  .progress-name{font-size:.75rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:70%}\n"
"  .progress-pct{font-family:var(--mono);font-size:.68rem;color:var(--muted)}\n"
"  .progress-bar-bg{height:4px;background:var(--border);border-radius:4px;overflow:hidden}\n"
"  .progress-bar-fill{height:100%;background:linear-gradient(90deg,var(--accent),var(--accent2));\n"
"    border-radius:4px;transition:width .2s}\n"
"  #toast-container{position:fixed;bottom:24px;left:50%;transform:translateX(-50%);\n"
"    display:flex;flex-direction:column;gap:8px;z-index:999;pointer-events:none}\n"
"  .toast{background:var(--surface);border:1px solid var(--border);border-radius:10px;\n"
"    padding:10px 18px;font-family:var(--mono);font-size:.75rem;white-space:nowrap;\n"
"    animation:slideUp .3s ease,fadeOut .4s ease 2.6s forwards}\n"
"  .toast.success{border-color:var(--success);color:var(--success)}\n"
"  .toast.error{border-color:var(--danger);color:var(--danger)}\n"
"  @keyframes slideUp{from{opacity:0;transform:translateY(12px)}to{opacity:1;transform:translateY(0)}}\n"
"  @keyframes fadeOut{to{opacity:0;transform:translateY(-8px)}}\n"
"  .empty-state{text-align:center;padding:32px 0 16px;font-family:var(--mono);font-size:.72rem;color:var(--muted)}\n"
"  .btn-refresh{background:none;border:1px solid var(--border);color:var(--muted);\n"
"    border-radius:8px;padding:5px 10px;font-family:var(--mono);font-size:.65rem;\n"
"    cursor:pointer;transition:all .15s;float:right}\n"
"  .btn-refresh:hover{border-color:var(--accent);color:var(--accent)}\n"
"  .section-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"  <div class='logo'>FileDrop</div>\n"
"  <div class='tagline'>Local wireless transfer &mdash; no accounts, no cloud</div>\n"
"</header>\n"
"<div class='grid'>\n"
"  <div class='panel upload'>\n"
"    <div class='panel-title'>&#8593; Upload to PC</div>\n"
"    <div class='dropzone' id='dropzone'>\n"
"      <input type='file' id='file-input' multiple>\n"
"      <span class='drop-icon'>&#128228;</span>\n"
"      <div class='drop-label'>Drop files here</div>\n"
"      <div class='drop-sub'>or tap to select</div>\n"
"    </div>\n"
"    <div class='progress-container' id='progress-container'></div>\n"
"  </div>\n"
"  <div class='panel download'>\n"
"    <div class='section-header'>\n"
"      <div class='panel-title'>&#8595; Download from PC</div>\n"
"      <button class='btn-refresh' onclick='loadFiles()'>&#8635; refresh</button>\n"
"    </div>\n"
"    <div class='file-list' id='file-list'>\n"
"      <div class='empty-state'>loading files...</div>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"<div id='toast-container'></div>\n"
"<script>\n"
"var dropzone=document.getElementById('dropzone');\n"
"var fileInput=document.getElementById('file-input');\n"
"var progressContainer=document.getElementById('progress-container');\n"
"['dragenter','dragover'].forEach(function(e){\n"
"  dropzone.addEventListener(e,function(ev){ev.preventDefault();dropzone.classList.add('drag-over');});\n"
"});\n"
"['dragleave','drop'].forEach(function(e){\n"
"  dropzone.addEventListener(e,function(ev){ev.preventDefault();dropzone.classList.remove('drag-over');});\n"
"});\n"
"dropzone.addEventListener('drop',function(ev){uploadFiles(Array.from(ev.dataTransfer.files));});\n"
"fileInput.addEventListener('change',function(){uploadFiles(Array.from(fileInput.files));fileInput.value='';});\n"
"function uploadFiles(files){files.forEach(function(f){uploadSingle(f);});}\n"
"function uploadSingle(file){\n"
"  var id='p_'+Math.random().toString(36).slice(2);\n"
"  var div=document.createElement('div');\n"
"  div.className='progress-item'; div.id=id;\n"
"  div.innerHTML=\n"
"    '<div class=\"progress-header\">'+\n"
"      '<span class=\"progress-name\">'+escHtml(file.name)+'</span>'+\n"
"      '<span class=\"progress-pct\" id=\"'+id+'_pct\">0%</span>'+\n"
"    '</div>'+\n"
"    '<div class=\"progress-bar-bg\"><div class=\"progress-bar-fill\" id=\"'+id+'_bar\" style=\"width:0%\"></div></div>';\n"
"  progressContainer.prepend(div);\n"
"  var xhr=new XMLHttpRequest();\n"
"  xhr.open('POST','/upload');\n"
"  xhr.upload.onprogress=function(e){\n"
"    if(e.lengthComputable){\n"
"      var pct=Math.round(e.loaded/e.total*100);\n"
"      document.getElementById(id+'_bar').style.width=pct+'%';\n"
"      document.getElementById(id+'_pct').textContent=pct+'%';\n"
"    }\n"
"  };\n"
"  xhr.onload=function(){\n"
"    if(xhr.status===200){\n"
"      document.getElementById(id+'_bar').style.background='var(--success)';\n"
"      document.getElementById(id+'_pct').textContent='\\u2713';\n"
"      toast(file.name+' uploaded!','success');\n"
"      setTimeout(function(){div.remove();},3000);\n"
"      loadFiles();\n"
"    }else{toast('Failed: '+file.name,'error');div.remove();}\n"
"  };\n"
"  xhr.onerror=function(){toast('Upload error','error');div.remove();};\n"
"  var fd=new FormData();\n"
"  fd.append('file',file);\n"
"  xhr.send(fd);\n"
"}\n"
"function escHtml(s){\n"
"  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');\n"
"}\n"
"function loadFiles(){\n"
"  var list=document.getElementById('file-list');\n"
"  fetch('/files').then(function(r){return r.json();}).then(function(files){\n"
"    if(!files.length){list.innerHTML='<div class=\"empty-state\">no files available for download</div>';return;}\n"
"    list.innerHTML=files.map(function(f){\n"
"      return '<div class=\"file-item\">'+\n"
"        '<span class=\"file-icon\">'+fileIcon(f.name)+'</span>'+\n"
"        '<div class=\"file-info\">'+\n"
"          '<div class=\"file-name\" title=\"'+escHtml(f.name)+'\">'+escHtml(f.name)+'</div>'+\n"
"          '<div class=\"file-size\">'+fmtSize(f.size)+'</div>'+\n"
"        '</div>'+\n"
"        '<a class=\"btn-download\" href=\"/download/'+encodeURIComponent(f.name)+'\" download=\"'+escHtml(f.name)+'\">&#8595; get</a>'+\n"
"      '</div>';\n"
"    }).join('');\n"
"  }).catch(function(){list.innerHTML='<div class=\"empty-state\">error loading files</div>';});\n"
"}\n"
"function fileIcon(name){\n"
"  var ext=name.split('.').pop().toLowerCase();\n"
"  var m={jpg:'\\uD83D\\uDDBC',jpeg:'\\uD83D\\uDDBC',png:'\\uD83D\\uDDBC',gif:'\\uD83D\\uDDBC',\n"
"         webp:'\\uD83D\\uDDBC',heic:'\\uD83D\\uDDBC',\n"
"         mp4:'\\uD83C\\uDFA6',mov:'\\uD83C\\uDFA6',avi:'\\uD83C\\uDFA6',mkv:'\\uD83C\\uDFA6',\n"
"         mp3:'\\uD83C\\uDFB5',wav:'\\uD83C\\uDFB5',flac:'\\uD83C\\uDFB5',m4a:'\\uD83C\\uDFB5',\n"
"         pdf:'\\uD83D\\uDCC4',doc:'\\uD83D\\uDCDD',docx:'\\uD83D\\uDCDD',txt:'\\uD83D\\uDCDD',md:'\\uD83D\\uDCDD',\n"
"         zip:'\\uD83D\\uDDDC',rar:'\\uD83D\\uDDDC',gz:'\\uD83D\\uDDDC',\n"
"         py:'\\uD83D\\uDC0D',js:'\\uD83D\\uDCDC',html:'\\uD83C\\uDF10',css:'\\uD83C\\uDFA8'};\n"
"  return m[ext]||'\\uD83D\\uDCC1';\n"
"}\n"
"function fmtSize(b){\n"
"  if(b<1024)return b+' B';\n"
"  if(b<1048576)return (b/1024).toFixed(1)+' KB';\n"
"  if(b<1073741824)return (b/1048576).toFixed(1)+' MB';\n"
"  return (b/1073741824).toFixed(2)+' GB';\n"
"}\n"
"function toast(msg,type){\n"
"  type=type||'success';\n"
"  var c=document.getElementById('toast-container');\n"
"  var t=document.createElement('div');\n"
"  t.className='toast '+type;\n"
"  t.textContent=msg;\n"
"  c.appendChild(t);\n"
"  setTimeout(function(){t.remove();},3200);\n"
"}\n"
"loadFiles();\n"
"setInterval(loadFiles,10000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* ── low-level network helpers ────────────────────────────────────────── */

/* memmem for Windows (not in MSVC/MinGW stdlib) */
static const void *mem_find(const void *h, size_t hn,
                              const void *n, size_t nn) {
    if (!nn) return h;
    if (hn < nn) return NULL;
    const unsigned char *hp = h, *np = n;
    for (size_t i = 0; i <= hn - nn; i++)
        if (hp[i] == np[0] && !memcmp(hp + i, np, nn))
            return hp + i;
    return NULL;
}

static void url_decode(char *s) {
    char *d = s;
    while (*s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) &&
                         isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *d++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else {
            *d++ = (*s == '+') ? ' ' : *s;
            s++;
        }
    }
    *d = 0;
}

static void json_escape(const char *src, char *dst, size_t cap) {
    size_t j = 0;
    for (const char *s = src; *s && j + 8 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if      (c == '"')  { dst[j++] = '\\'; dst[j++] = '"';  }
        else if (c == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (c < 0x20)  { j += snprintf(dst+j, cap-j, "\\u%04x", c); }
        else                  dst[j++] = c;
    }
    dst[j] = 0;
}

static int sock_send(SOCKET s, const void *buf, int len) {
    const char *p = buf;
    int sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

static int sock_sendf(SOCKET s, const char *fmt, ...) {
    char tmp[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    return sock_send(s, tmp, n);
}

/* ── HTTP helpers ─────────────────────────────────────────────────────── */

static void send_header(SOCKET s, int code, const char *status,
                        const char *ctype, long long clen,
                        const char *extra) {
    sock_sendf(s,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "%s"
        "\r\n",
        code, status, ctype, clen, extra ? extra : "");
}

static void send_text(SOCKET s, int code, const char *status,
                      const char *body) {
    int len = (int)strlen(body);
    send_header(s, code, status, "text/plain; charset=utf-8", len, NULL);
    sock_send(s, body, len);
}

/* ── Local IP ─────────────────────────────────────────────────────────── */

static void get_local_ip(char *out, size_t len) {
    strncpy(out, "127.0.0.1", len);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &sa.sin_addr);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        int sl = sizeof(sa);
        getsockname(s, (struct sockaddr *)&sa, &sl);
        inet_ntop(AF_INET, &sa.sin_addr, out, (socklen_t)len);
    }
    closesocket(s);
}

/* ── Request parser ───────────────────────────────────────────────────── */

typedef struct {
    char   method[16];
    char   path[4096];
    char   content_type[512];
    long long content_length;
    int    body_off;   /* byte offset of body in the read buffer */
    int    total_read; /* total bytes in read buffer             */
} Request;

/* Read from socket until we see \r\n\r\n or buf is full.
   Returns total bytes read; sets req->body_off.               */
static int read_request(SOCKET sk, char *buf, int bufsz, Request *req) {
    int total = 0;
    req->body_off = -1;

    while (total < bufsz - 1) {
        int n = recv(sk, buf + total,
                     (bufsz - total - 1) < IO_CAP
                         ? (bufsz - total - 1) : IO_CAP,
                     0);
        if (n <= 0) break;
        total += n;
        buf[total] = 0;
        const char *p = (const char *)mem_find(buf, total, "\r\n\r\n", 4);
        if (p) { req->body_off = (int)(p - buf) + 4; break; }
    }

    req->total_read = total;
    if (req->body_off < 0) return total; /* incomplete */

    /* --- parse request line --- */
    const char *cur = buf;
    const char *sp  = strchr(cur, ' ');
    if (!sp) return total;

    int ml = (int)(sp - cur);
    if (ml >= (int)sizeof(req->method)) ml = (int)sizeof(req->method) - 1;
    memcpy(req->method, cur, ml);
    req->method[ml] = 0;

    cur = sp + 1;
    sp  = strchr(cur, ' ');
    if (!sp) return total;

    int pl = (int)(sp - cur);
    if (pl >= (int)sizeof(req->path)) pl = (int)sizeof(req->path) - 1;
    memcpy(req->path, cur, pl);
    req->path[pl] = 0;
    url_decode(req->path);

    /* --- parse headers --- */
    cur = strstr(buf, "\r\n");
    if (!cur) return total;
    cur += 2; /* skip request line */

    while (cur < buf + req->body_off - 2) {
        const char *eol = strstr(cur, "\r\n");
        if (!eol || eol == cur) break;

        /* split at colon */
        const char *colon = (const char *)memchr(cur, ':', eol - cur);
        if (colon) {
            char name[64];
            int nl = (int)(colon - cur);
            if (nl >= (int)sizeof(name)) nl = (int)sizeof(name) - 1;
            memcpy(name, cur, nl); name[nl] = 0;
            for (char *c = name; *c; c++) *c = (char)tolower((unsigned char)*c);

            const char *val = colon + 1;
            while (*val == ' ') val++;

            if (strcmp(name, "content-length") == 0) {
                req->content_length = (long long)strtoll(val, NULL, 10);
            } else if (strcmp(name, "content-type") == 0) {
                int vl = (int)(eol - val);
                if (vl >= (int)sizeof(req->content_type))
                    vl = (int)sizeof(req->content_type) - 1;
                memcpy(req->content_type, val, vl);
                req->content_type[vl] = 0;
            }
        }
        cur = eol + 2;
    }
    return total;
}

/* ── Route: GET / ─────────────────────────────────────────────────────── */

static void route_index(SOCKET sk) {
    int len = (int)strlen(HTML);
    send_header(sk, 200, "OK", "text/html; charset=utf-8", len, NULL);
    sock_send(sk, HTML, len);
}

/* ── Route: GET /files ────────────────────────────────────────────────── */

/* qsort comparator – case-insensitive filename sort */
static int cmp_fname(const void *a, const void *b) {
    return _stricmp(*(const char *const *)a, *(const char *const *)b);
}

static void route_files(SOCKET sk) {
    /* collect names + sizes */
    enum { MAX_FILES = 4096 };
    static char  names[MAX_FILES][MAX_PATH];
    static long long sizes[MAX_FILES];
    static const char *ptrs[MAX_FILES];
    int count = 0;

    char pattern[MAX_PATH + 4];
    snprintf(pattern, sizeof(pattern), "%s\\*", g_dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (fd.cFileName[0] == '.') continue;
            if (count >= MAX_FILES) break;

            memcpy(names[count], fd.cFileName,
                   strnlen(fd.cFileName, MAX_PATH - 1));
            names[count][strnlen(fd.cFileName, MAX_PATH - 1)] = 0;
            LARGE_INTEGER sz;
            sz.LowPart  = fd.nFileSizeLow;
            sz.HighPart = (LONG)fd.nFileSizeHigh;
            sizes[count] = sz.QuadPart;
            ptrs[count]  = names[count];
            count++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    /* sort */
    if (count > 1)
        qsort(ptrs, count, sizeof(ptrs[0]), cmp_fname);

    /* build JSON */
    size_t jcap = (size_t)count * (MAX_PATH * 3 + 64) + 8;
    if (jcap < 64) jcap = 64;
    char *json = (char *)malloc(jcap);
    if (!json) { send_text(sk, 500, "Internal Server Error", "OOM"); return; }

    int jlen = 0;
    json[jlen++] = '[';
    for (int i = 0; i < count; i++) {
        /* find original sizes[] index via pointer arithmetic */
        int idx = (int)(ptrs[i] - names[0]) / MAX_PATH;
        char esc[MAX_PATH * 2];
        json_escape(ptrs[i], esc, sizeof(esc));
        if (i) json[jlen++] = ',';
        jlen += snprintf(json + jlen, jcap - jlen,
                         "{\"name\":\"%s\",\"size\":%lld}", esc, sizes[idx]);
    }
    json[jlen++] = ']';
    json[jlen]   = 0;

    send_header(sk, 200, "OK", "application/json", jlen, NULL);
    sock_send(sk, json, jlen);
    free(json);
}

/* ── Route: GET /download/<name> ─────────────────────────────────────── */

static void route_download(SOCKET sk, const char *raw) {
    /* basename only (security) */
    const char *base = strrchr(raw, '/');
    base = base ? base + 1 : raw;
    if (!*base) { send_text(sk, 400, "Bad Request", "Empty filename"); return; }

    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s\\%s", g_dir, base);

    FILE *fp = fopen(full, "rb");
    if (!fp) { send_text(sk, 404, "Not Found", "File not found"); return; }

    /* file size */
    _fseeki64(fp, 0, SEEK_END);
    long long fsz = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);

    char extra[512];
    /* percent-encode the filename for Content-Disposition */
    char enc_name[MAX_PATH * 3];
    {
        const char *q = base;
        char *e = enc_name;
        while (*q) {
            unsigned char c = (unsigned char)*q++;
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                *e++ = c;
            } else {
                e += sprintf(e, "%%%02X", c);
            }
        }
        *e = 0;
    }
    snprintf(extra, sizeof(extra),
             "Content-Disposition: attachment; filename=\"%s\"\r\n", enc_name);

    send_header(sk, 200, "OK", "application/octet-stream", fsz, extra);

    char *buf = (char *)malloc(IO_CAP);
    if (!buf) { fclose(fp); return; }

    size_t n;
    while ((n = fread(buf, 1, IO_CAP, fp)) > 0) {
        if (sock_send(sk, buf, (int)n) < 0) break;
    }
    free(buf);
    fclose(fp);
}

/* ── Route: POST /upload (streaming multipart parser) ────────────────── */

static void route_upload(SOCKET sk, Request *req,
                          const char *prebuf, int prebuf_len) {
    /* ── 1. extract boundary ── */
    const char *bp = strstr(req->content_type, "boundary=");
    if (!bp) { send_text(sk, 400, "Bad Request", "No boundary"); return; }
    bp += 9;

    char boundary[256] = { 0 };
    if (*bp == '"') {
        bp++;
        const char *end = strchr(bp, '"');
        int l = end ? (int)(end - bp) : (int)strlen(bp);
        if (l > 255) l = 255;
        memcpy(boundary, bp, l);
    } else {
        int i = 0;
        while (*bp && *bp != ';' && *bp != ' ' && *bp != '\r' && *bp != '\n'
               && i < 255)
            boundary[i++] = *bp++;
    }
    if (!boundary[0]) { send_text(sk, 400, "Bad Request", "Empty boundary"); return; }

    /* end marker = "\r\n--<boundary>" */
    char em[512];
    int  emlen = snprintf(em, sizeof(em), "\r\n--%s", boundary);

    /* ── 2. working buffer ── */
    int wbcap = IO_CAP * 2 + emlen + 64;
    char *wb  = (char *)malloc(wbcap);
    if (!wb) { send_text(sk, 500, "Internal Server Error", "OOM"); return; }

    /* seed with already-read body bytes */
    int    wblen    = prebuf_len < wbcap ? prebuf_len : wbcap;
    long long remaining = req->content_length - prebuf_len;
    memcpy(wb, prebuf, wblen);

    /* ── 3. helper: read more into wb from socket ── */
#define REFILL() do { \
    if (remaining > 0 && wblen < wbcap) { \
        int _tr = wbcap - wblen; \
        if (_tr > IO_CAP) _tr = IO_CAP; \
        if ((long long)_tr > remaining) _tr = (int)remaining; \
        int _n = recv(sk, wb + wblen, _tr, 0); \
        if (_n > 0) { wblen += _n; remaining -= _n; } \
    } \
} while(0)

    /* ── 4. find end of multipart part-headers (\r\n\r\n) ── */
    while (!mem_find(wb, wblen, "\r\n\r\n", 4) && remaining > 0)
        REFILL();

    const char *eoh = (const char *)mem_find(wb, wblen, "\r\n\r\n", 4);
    if (!eoh) {
        free(wb);
        send_text(sk, 400, "Bad Request", "Malformed multipart");
        return;
    }

    /* ── 5. extract filename from part headers ── */
    char filename[MAX_PATH] = { 0 };
    const char *cd = (const char *)mem_find(wb, eoh - wb,
                                             "filename=\"", 10);
    if (cd) {
        cd += 10;
        const char *end = (const char *)memchr(cd, '"', eoh - cd);
        if (end) {
            int l = (int)(end - cd);
            if (l >= MAX_PATH) l = MAX_PATH - 1;
            memcpy(filename, cd, l);
            filename[l] = 0;
        }
    }
    if (!filename[0]) strcpy(filename, "upload.bin");

    /* basename only */
    {
        char *p = strrchr(filename, '/');
        if (p) memmove(filename, p + 1, strlen(p));
        p = strrchr(filename, '\\');
        if (p) memmove(filename, p + 1, strlen(p));
    }

    /* ── 6. resolve output path (avoid overwriting) ── */
    char outpath[MAX_PATH];
    snprintf(outpath, sizeof(outpath), "%s\\%s", g_dir, filename);

    char stem[MAX_PATH], ext[64];
    {
        char *dot = strrchr(filename, '.');
        if (dot) {
            int sl = (int)(dot - filename);
            memcpy(stem, filename, sl); stem[sl] = 0;
            strncpy(ext, dot, sizeof(ext) - 1);
        } else {
            memcpy(stem, filename, sizeof(stem) - 1);
            stem[sizeof(stem) - 1] = 0;
            ext[0] = 0;
        }
    }
    int counter = 1;
    while (GetFileAttributesA(outpath) != INVALID_FILE_ATTRIBUTES) {
        snprintf(outpath, sizeof(outpath), "%s\\%s_%d%s",
                 g_dir, stem, counter++, ext);
    }

    FILE *fp = fopen(outpath, "wb");
    if (!fp) {
        free(wb);
        send_text(sk, 500, "Internal Server Error", "Cannot create file");
        return;
    }

    /* ── 7. stream file data ── */
    /* shift working buffer: file data starts after \r\n\r\n */
    int file_start = (int)(eoh + 4 - wb);
    memmove(wb, wb + file_start, wblen - file_start);
    wblen -= file_start;

    long long bytes_written = 0;

    while (1) {
        /* search for end marker in current buffer */
        const char *found = (const char *)mem_find(wb, wblen, em, emlen);
        if (found) {
            int chunk = (int)(found - wb);
            fwrite(wb, 1, chunk, fp);
            bytes_written += chunk;
            break;
        }

        /* no more socket data? flush & stop */
        if (remaining <= 0) {
            fwrite(wb, 1, wblen, fp);
            bytes_written += wblen;
            break;
        }

        /* write safe portion (keep last emlen-1 bytes for split-marker) */
        int safe = wblen - (emlen - 1);
        if (safe > 0) {
            fwrite(wb, 1, safe, fp);
            bytes_written += safe;
            memmove(wb, wb + safe, wblen - safe);
            wblen -= safe;
        }

        REFILL();
    }

#undef REFILL

    fclose(fp);
    free(wb);

    const char *bname = strrchr(outpath, '\\');
    printf("  OK  %-40s  (%lld bytes)\n",
           bname ? bname + 1 : outpath, bytes_written);
    fflush(stdout);

    send_text(sk, 200, "OK", "OK");
}

/* ── Client thread ────────────────────────────────────────────────────── */

typedef struct { SOCKET sk; } ClientCtx;

static DWORD WINAPI client_thread(LPVOID arg) {
    SOCKET sk = ((ClientCtx *)arg)->sk;
    free(arg);

    char *hbuf = (char *)malloc(HDR_CAP);
    if (!hbuf) { closesocket(sk); return 0; }

    Request req;
    memset(&req, 0, sizeof(req));

    read_request(sk, hbuf, HDR_CAP, &req);

    if (req.body_off < 0 || !req.method[0]) {
        free(hbuf);
        closesocket(sk);
        return 0;
    }

    const char *prebuf    = hbuf + req.body_off;
    int          prebuflen = req.total_read - req.body_off;

    /* ── dispatch ── */
    if (strcmp(req.method, "GET") == 0) {

        if (strcmp(req.path, "/") == 0) {
            route_index(sk);

        } else if (strcmp(req.path, "/files") == 0) {
            route_files(sk);

        } else if (strncmp(req.path, "/download/", 10) == 0) {
            route_download(sk, req.path + 10);

        } else {
            send_text(sk, 404, "Not Found", "Not found");
        }

    } else if (strcmp(req.method, "POST") == 0) {

        if (strcmp(req.path, "/upload") == 0) {
            route_upload(sk, &req, prebuf, prebuflen);
        } else {
            send_text(sk, 404, "Not Found", "Not found");
        }

    } else if (strcmp(req.method, "OPTIONS") == 0) {
        send_header(sk, 204, "No Content", "text/plain", 0,
                    "Allow: GET, POST, OPTIONS\r\n");
    } else {
        send_text(sk, 405, "Method Not Allowed", "Method not allowed");
    }

    free(hbuf);
    closesocket(sk);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* defaults */
    g_port = PORT_DEFAULT;
    GetCurrentDirectoryA(sizeof(g_dir), g_dir);

    /* parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            puts("Usage: filedrop.exe [directory] [--port N]");
            return 0;
        } else if (argv[i][0] != '-') {
            strncpy(g_dir, argv[i], sizeof(g_dir) - 1);
        }
    }

    /* resolve to absolute path */
    {
        char abs[MAX_PATH];
        if (GetFullPathNameA(g_dir, MAX_PATH, abs, NULL)) {
            memcpy(g_dir, abs, MAX_PATH - 1);
            g_dir[MAX_PATH - 1] = 0;
        }
    }

    /* create directory if it doesn't exist */
    CreateDirectoryA(g_dir, NULL);

    /* init winsock */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    /* create listening socket */
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port        = htons((u_short)g_port);

    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed – is port %d already in use?\n", g_port);
        closesocket(srv);
        WSACleanup();
        return 1;
    }

    listen(srv, 32);

    char ip[64];
    get_local_ip(ip, sizeof(ip));

    printf(
        "\n"
        "  +------------------------------------------+\n"
        "  |          FileDrop is running             |\n"
        "  +------------------------------------------+\n"
        "  |  Open on your phone:                     |\n"
        "  |    http://%s:%-5d                  |\n"
        "  |                                          |\n"
        "  |  Serving:                                |\n"
        "  |    %s\n"
        "  |                                          |\n"
        "  |  Press Ctrl-C to stop                    |\n"
        "  +------------------------------------------+\n\n",
        ip, g_port, g_dir);
    fflush(stdout);

    /* accept loop */
    while (1) {
        SOCKET client = accept(srv, NULL, NULL);
        if (client == INVALID_SOCKET) continue;

        ClientCtx *ctx = (ClientCtx *)malloc(sizeof(ClientCtx));
        if (!ctx) { closesocket(client); continue; }
        ctx->sk = client;

        HANDLE t = CreateThread(NULL, 0, client_thread, ctx, 0, NULL);
        if (t) CloseHandle(t);
        else  { free(ctx); closesocket(client); }
    }

    closesocket(srv);
    WSACleanup();
    return 0;
}