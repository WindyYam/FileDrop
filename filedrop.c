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

#ifdef _MSC_VER
#  pragma comment(lib, "ws2_32.lib")
#endif

/* ── tunables ─────────────────────────────────────────────────────────── */
#define PORT_DEFAULT  8000
#define HDR_CAP       16384
#define IO_CAP        65536

static char g_dir[MAX_PATH];
static int  g_port = PORT_DEFAULT;

/* ── HTML page (Windows 95 aesthetic) ────────────────────────────────── */
static const char HTML[] =
"<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
"<html>\n"
"<head>\n"
"<meta charset='UTF-8'>\n"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>\n"
"<title>FileDrop v1.0 - File Transfer Utility</title>\n"
"<style>\n"
"  body{\n"
"    background-color:#008080;\n"
"    background-image:url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='4' height='4'%3E%3Crect width='2' height='2' fill='%23007070'/%3E%3Crect x='2' y='2' width='2' height='2' fill='%23007070'/%3E%3C/svg%3E\");\n"
"    font-family:'MS Sans Serif',Arial,sans-serif;\n"
"    font-size:11px;margin:0;padding:12px;color:#000;\n"
"  }\n"
"  .window{background:#d4d0c8;border-top:2px solid #fff;border-left:2px solid #fff;\n"
"    border-right:2px solid #808080;border-bottom:2px solid #808080;margin-bottom:10px}\n"
"  .title-bar{background:linear-gradient(90deg,#000080,#1084d0);color:#fff;\n"
"    font-weight:bold;font-size:11px;padding:3px 6px;\n"
"    display:flex;align-items:center;justify-content:space-between;user-select:none}\n"
"  .title-bar-text{display:flex;align-items:center;gap:6px}\n"
"  .title-bar-buttons{display:flex;gap:2px}\n"
"  .tb-btn{width:16px;height:14px;background:#d4d0c8;\n"
"    border-top:1px solid #fff;border-left:1px solid #fff;\n"
"    border-right:1px solid #808080;border-bottom:1px solid #808080;\n"
"    font-size:9px;cursor:pointer;display:flex;align-items:center;justify-content:center;color:#000}\n"
"  .window-body{padding:10px}\n"
"  .groupbox{border:1px solid #808080;border-top:none;padding:8px;margin-top:10px;position:relative}\n"
"  .groupbox-label{position:absolute;top:-8px;left:8px;background:#d4d0c8;padding:0 4px;font-weight:bold}\n"
"  button,.btn{background:#d4d0c8;\n"
"    border-top:2px solid #fff;border-left:2px solid #fff;\n"
"    border-right:2px solid #808080;border-bottom:2px solid #808080;\n"
"    padding:3px 12px;font-family:'MS Sans Serif',Arial,sans-serif;\n"
"    font-size:11px;cursor:pointer;color:#000;text-decoration:none;\n"
"    display:inline-block;white-space:nowrap}\n"
"  button:active{border-top:2px solid #808080;border-left:2px solid #808080;\n"
"    border-right:2px solid #fff;border-bottom:2px solid #fff}\n"
"  .dropzone{border-top:1px solid #808080;border-left:1px solid #808080;\n"
"    border-right:1px solid #fff;border-bottom:1px solid #fff;\n"
"    background:#fff;padding:20px;text-align:center;cursor:pointer;\n"
"    position:relative;min-height:80px;\n"
"    display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px}\n"
"  .dropzone.drag-over{background:#000080;color:#fff}\n"
"  .dropzone input[type=file]{position:absolute;inset:0;opacity:0;cursor:pointer;width:100%;height:100%}\n"
"  .drop-icon{font-size:2rem;line-height:1}\n"
"  .drop-label{font-weight:bold}\n"
"  .drop-sub{color:#444;font-size:10px}\n"
"  .dropzone.drag-over .drop-sub{color:#aad}\n"
"  .file-list{max-height:220px;overflow-y:auto;\n"
"    border-top:1px solid #808080;border-left:1px solid #808080;\n"
"    border-right:1px solid #fff;border-bottom:1px solid #fff;background:#fff}\n"
"  .file-item{display:flex;align-items:center;gap:6px;\n"
"    padding:3px 6px;border-bottom:1px solid #e8e4dc;cursor:default}\n"
"  .file-item:hover{background:#000080;color:#fff}\n"
"  .file-item:hover .file-size{color:#aad}\n"
"  .file-item:hover .btn-download{color:#ff0}\n"
"  .file-icon{font-size:1rem;flex-shrink:0}\n"
"  .file-info{flex:1;min-width:0;overflow:hidden}\n"
"  .file-name{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-weight:bold}\n"
"  .file-size{font-size:10px;color:#666}\n"
"  .btn-download{background:none;border:none;padding:0;font-size:10px;\n"
"    color:#000080;cursor:pointer;text-decoration:underline;\n"
"    font-family:'MS Sans Serif',Arial,sans-serif;flex-shrink:0}\n"
"  .progress-container{margin-top:8px;display:flex;flex-direction:column;gap:4px}\n"
"  .progress-header{display:flex;justify-content:space-between;font-size:10px;margin-bottom:2px}\n"
"  .progress-name{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:75%}\n"
"  .progress-pct{color:#444}\n"
"  .progress-bar-bg{height:12px;\n"
"    border-top:1px solid #808080;border-left:1px solid #808080;\n"
"    border-right:1px solid #fff;border-bottom:1px solid #fff;\n"
"    background:#d4d0c8;overflow:hidden}\n"
"  .progress-bar-fill{height:100%;\n"
"    background:repeating-linear-gradient(90deg,#000080 0px,#000080 8px,#1084d0 8px,#1084d0 16px);\n"
"    transition:width .2s}\n"
"  .statusbar{background:#d4d0c8;border-top:1px solid #808080;\n"
"    padding:2px 6px;font-size:10px;color:#444;display:flex;gap:8px}\n"
"  .status-cell{border-top:1px solid #808080;border-left:1px solid #808080;\n"
"    border-right:1px solid #fff;border-bottom:1px solid #fff;padding:1px 6px}\n"
"  .layout{display:grid;grid-template-columns:1fr 1fr;gap:10px;max-width:860px;margin:0 auto}\n"
"  @media(max-width:600px){.layout{grid-template-columns:1fr}}\n"
"  .app-header{max-width:860px;margin:0 auto 10px}\n"
"  .menubar{background:#d4d0c8;\n"
"    border-top:1px solid #fff;border-left:1px solid #fff;\n"
"    border-right:1px solid #808080;border-bottom:1px solid #808080;\n"
"    padding:2px 4px;font-size:11px;display:flex;gap:2px;align-items:center}\n"
"  .menu-item{padding:2px 8px;cursor:default}\n"
"  .menu-item:hover{background:#000080;color:#fff}\n"
"  .toolbar{background:#d4d0c8;border-top:1px solid #fff;border-left:1px solid #fff;\n"
"    padding:3px 4px;display:flex;gap:4px;align-items:center;font-size:10px;color:#444}\n"
"  .separator{width:1px;height:20px;border-left:1px solid #808080;border-right:1px solid #fff;margin:0 2px}\n"
"  .empty-state{padding:20px;text-align:center;color:#666;font-style:italic}\n"
"  .banner{background:#000080;color:#ff0;font-weight:bold;font-size:11px;\n"
"    padding:2px 0;overflow:hidden;white-space:nowrap}\n"
"  #toast-container{position:fixed;bottom:16px;right:16px;\n"
"    display:flex;flex-direction:column;gap:6px;z-index:999;pointer-events:none}\n"
"  .toast{background:#d4d0c8;\n"
"    border-top:2px solid #fff;border-left:2px solid #fff;\n"
"    border-right:2px solid #808080;border-bottom:2px solid #808080;\n"
"    padding:6px 14px;font-size:11px;display:flex;align-items:center;gap:8px;\n"
"    animation:popIn .15s ease,fadeOut .3s ease 2.7s forwards;min-width:180px}\n"
"  @keyframes popIn{from{transform:scale(.9);opacity:0}to{transform:scale(1);opacity:1}}\n"
"  @keyframes fadeOut{to{opacity:0;transform:translateY(6px)}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class='app-header'>\n"
"  <div class='window' style='margin-bottom:0'>\n"
"    <div class='title-bar'>\n"
"      <div class='title-bar-text'>\n"
"        <span>&#128193;</span> FileDrop v1.0 - Wireless File Transfer Utility\n"
"      </div>\n"
"      <div class='title-bar-buttons'>\n"
"        <span class='tb-btn'>_</span>\n"
"        <span class='tb-btn'>&#9633;</span>\n"
"        <span class='tb-btn'>&#10005;</span>\n"
"      </div>\n"
"    </div>\n"
"    <div class='menubar'>\n"
"      <span class='menu-item'><u>F</u>ile</span>\n"
"      <span class='menu-item'><u>E</u>dit</span>\n"
"      <span class='menu-item'><u>V</u>iew</span>\n"
"      <span class='menu-item'><u>T</u>ransfer</span>\n"
"      <span class='menu-item'><u>H</u>elp</span>\n"
"    </div>\n"
"    <div class='toolbar'>\n"
"      <button onclick='loadFiles()' style='padding:2px 8px;font-size:10px;'>&#128260; Refresh</button>\n"
"      <div class='separator'></div>\n"
"      <span>&#128187; Local Wi-Fi Transfer &nbsp;&mdash;&nbsp; No internet required</span>\n"
"      <div class='separator'></div>\n"
"      <span id='toolbar-status'>Ready.</span>\n"
"    </div>\n"
"    <div class='banner'>\n"
"      <marquee scrollamount='3'>*** Welcome to FileDrop v1.0 *** Transfer files between PC and phone over your local network *** No internet required *** No accounts needed *** FREE SOFTWARE *** Have a nice day! ***</marquee>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"<div class='layout'>\n"
"  <div class='window'>\n"
"    <div class='title-bar'>\n"
"      <div class='title-bar-text'><span>&#128228;</span> Send Files to PC</div>\n"
"    </div>\n"
"    <div class='window-body'>\n"
"      <div class='groupbox' style='margin-top:14px'>\n"
"        <span class='groupbox-label'>Drop Zone</span>\n"
"        <div class='dropzone' id='dropzone'>\n"
"          <input type='file' id='file-input' multiple>\n"
"          <span class='drop-icon'>&#128190;</span>\n"
"          <div class='drop-label'>Drop files here to upload</div>\n"
"          <div class='drop-sub'>- or click to browse -</div>\n"
"        </div>\n"
"      </div>\n"
"      <div class='progress-container' id='progress-container'></div>\n"
"    </div>\n"
"    <div class='statusbar'>\n"
"      <span class='status-cell' id='upload-status'>Awaiting files...</span>\n"
"    </div>\n"
"  </div>\n"
"  <div class='window'>\n"
"    <div class='title-bar'>\n"
"      <div class='title-bar-text'><span>&#128229;</span> Get Files from PC</div>\n"
"    </div>\n"
"    <div class='window-body'>\n"
"      <div class='groupbox' style='margin-top:14px'>\n"
"        <span class='groupbox-label'>Available Files</span>\n"
"        <div class='file-list' id='file-list'>\n"
"          <div class='empty-state'>Scanning directory...</div>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"    <div class='statusbar'>\n"
"      <span class='status-cell' id='download-status'>Loading...</span>\n"
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
"  div.className='progress-item';div.id=id;\n"
"  div.innerHTML=\n"
"    '<div class=\"progress-header\">'+\n"
"      '<span class=\"progress-name\">'+escHtml(file.name)+'</span>'+\n"
"      '<span class=\"progress-pct\" id=\"'+id+'_pct\">0%</span>'+\n"
"    '</div>'+\n"
"    '<div class=\"progress-bar-bg\"><div class=\"progress-bar-fill\" id=\"'+id+'_bar\" style=\"width:0%\"></div></div>';\n"
"  progressContainer.prepend(div);\n"
"  document.getElementById('upload-status').textContent='Uploading: '+file.name;\n"
"  document.getElementById('toolbar-status').textContent='Transferring...';\n"
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
"      document.getElementById(id+'_bar').style.backgroundImage='none';\n"
"      document.getElementById(id+'_bar').style.background='#008000';\n"
"      document.getElementById(id+'_pct').textContent='Done';\n"
"      document.getElementById('upload-status').textContent='Last: '+file.name;\n"
"      document.getElementById('toolbar-status').textContent='Ready.';\n"
"      toast('\\u2713 '+file.name+' uploaded!','success');\n"
"      setTimeout(function(){div.remove();},3000);\n"
"      loadFiles();\n"
"    }else{toast('\\u2717 Failed: '+file.name,'error');div.remove();\n"
"      document.getElementById('toolbar-status').textContent='Error.';}\n"
"  };\n"
"  xhr.onerror=function(){toast('\\u2717 Upload error','error');div.remove();};\n"
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
"    document.getElementById('download-status').textContent=files.length+' file(s) available';\n"
"    if(!files.length){list.innerHTML='<div class=\"empty-state\">(No files in directory)</div>';return;}\n"
"    list.innerHTML=files.map(function(f){\n"
"      return '<div class=\"file-item\">'+\n"
"        '<span class=\"file-icon\">'+fileIcon(f.name)+'</span>'+\n"
"        '<div class=\"file-info\">'+\n"
"          '<div class=\"file-name\" title=\"'+escHtml(f.name)+'\">'+escHtml(f.name)+'</div>'+\n"
"          '<div class=\"file-size\">'+fmtSize(f.size)+'</div>'+\n"
"        '</div>'+\n"
"        '<a class=\"btn-download\" href=\"/download/'+encodeURIComponent(f.name)+'\" download=\"'+escHtml(f.name)+'\">[Save]</a>'+\n"
"      '</div>';\n"
"    }).join('');\n"
"  }).catch(function(){\n"
"    list.innerHTML='<div class=\"empty-state\">Error reading directory.</div>';\n"
"    document.getElementById('download-status').textContent='Error.';\n"
"  });\n"
"}\n"
"function fileIcon(name){\n"
"  var ext=name.split('.').pop().toLowerCase();\n"
"  var m={jpg:'&#128444;',jpeg:'&#128444;',png:'&#128444;',gif:'&#128444;',bmp:'&#128444;',heic:'&#128444;',\n"
"         mp4:'&#127916;',mov:'&#127916;',avi:'&#127916;',mkv:'&#127916;',\n"
"         mp3:'&#127925;',wav:'&#127925;',flac:'&#127925;',m4a:'&#127925;',mid:'&#127925;',\n"
"         pdf:'&#128196;',doc:'&#128221;',docx:'&#128221;',txt:'&#128196;',md:'&#128196;',\n"
"         zip:'&#128230;',rar:'&#128230;',gz:'&#128230;',\n"
"         exe:'&#9881;',bat:'&#9881;',\n"
"         py:'&#128013;',js:'&#128221;',html:'&#127760;',css:'&#127912;'};\n"
"  return m[ext]||'&#128193;';\n"
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
"  t.innerHTML=msg;\n"
"  c.appendChild(t);\n"
"  setTimeout(function(){t.remove();},3200);\n"
"}\n"
"loadFiles();\n"
"setInterval(loadFiles,10000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* ── low-level network helpers ────────────────────────────────────────── */

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
    int    body_off;
    int    total_read;
} Request;

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
    if (req->body_off < 0) return total;

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

    cur = strstr(buf, "\r\n");
    if (!cur) return total;
    cur += 2;

    while (cur < buf + req->body_off - 2) {
        const char *eol = strstr(cur, "\r\n");
        if (!eol || eol == cur) break;

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

static int cmp_fname(const void *a, const void *b) {
    return _stricmp(*(const char *const *)a, *(const char *const *)b);
}

static void route_files(SOCKET sk) {
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

    if (count > 1)
        qsort(ptrs, count, sizeof(ptrs[0]), cmp_fname);

    size_t jcap = (size_t)count * (MAX_PATH * 3 + 64) + 8;
    if (jcap < 64) jcap = 64;
    char *json = (char *)malloc(jcap);
    if (!json) { send_text(sk, 500, "Internal Server Error", "OOM"); return; }

    int jlen = 0;
    json[jlen++] = '[';
    for (int i = 0; i < count; i++) {
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
    const char *base = strrchr(raw, '/');
    base = base ? base + 1 : raw;
    if (!*base) { send_text(sk, 400, "Bad Request", "Empty filename"); return; }

    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s\\%s", g_dir, base);

    FILE *fp = fopen(full, "rb");
    if (!fp) { send_text(sk, 404, "Not Found", "File not found"); return; }

    _fseeki64(fp, 0, SEEK_END);
    long long fsz = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);

    char extra[512];
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

    char em[512];
    int  emlen = snprintf(em, sizeof(em), "\r\n--%s", boundary);

    int wbcap = IO_CAP * 2 + emlen + 64;
    char *wb  = (char *)malloc(wbcap);
    if (!wb) { send_text(sk, 500, "Internal Server Error", "OOM"); return; }

    int    wblen    = prebuf_len < wbcap ? prebuf_len : wbcap;
    long long remaining = req->content_length - prebuf_len;
    memcpy(wb, prebuf, wblen);

#define REFILL() do { \
    if (remaining > 0 && wblen < wbcap) { \
        int _tr = wbcap - wblen; \
        if (_tr > IO_CAP) _tr = IO_CAP; \
        if ((long long)_tr > remaining) _tr = (int)remaining; \
        int _n = recv(sk, wb + wblen, _tr, 0); \
        if (_n > 0) { wblen += _n; remaining -= _n; } \
    } \
} while(0)

    while (!mem_find(wb, wblen, "\r\n\r\n", 4) && remaining > 0)
        REFILL();

    const char *eoh = (const char *)mem_find(wb, wblen, "\r\n\r\n", 4);
    if (!eoh) {
        free(wb);
        send_text(sk, 400, "Bad Request", "Malformed multipart");
        return;
    }

    char filename[MAX_PATH] = { 0 };
    const char *cd = (const char *)mem_find(wb, eoh - wb, "filename=\"", 10);
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

    {
        char *p = strrchr(filename, '/');
        if (p) memmove(filename, p + 1, strlen(p));
        p = strrchr(filename, '\\');
        if (p) memmove(filename, p + 1, strlen(p));
    }

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

    int file_start = (int)(eoh + 4 - wb);
    memmove(wb, wb + file_start, wblen - file_start);
    wblen -= file_start;

    long long bytes_written = 0;

    while (1) {
        const char *found = (const char *)mem_find(wb, wblen, em, emlen);
        if (found) {
            int chunk = (int)(found - wb);
            fwrite(wb, 1, chunk, fp);
            bytes_written += chunk;
            break;
        }

        if (remaining <= 0) {
            fwrite(wb, 1, wblen, fp);
            bytes_written += wblen;
            break;
        }

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
    g_port = PORT_DEFAULT;
    GetCurrentDirectoryA(sizeof(g_dir), g_dir);

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

    {
        char abs[MAX_PATH];
        if (GetFullPathNameA(g_dir, MAX_PATH, abs, NULL)) {
            memcpy(g_dir, abs, MAX_PATH - 1);
            g_dir[MAX_PATH - 1] = 0;
        }
    }

    CreateDirectoryA(g_dir, NULL);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

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
        fprintf(stderr, "bind() failed - is port %d already in use?\n", g_port);
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