#pragma once
// ============================================================================
// tui.hpp  -  방향키 기반 터미널 UI (뷰포트 스크롤 지원)
// ============================================================================

#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

namespace tui_detail {

inline void set_raw(termios& old_t) {
    termios t; tcgetattr(STDIN_FILENO, &t); old_t = t;
    t.c_lflag &= ~(ICANON | ECHO); t.c_cc[VMIN]=1; t.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    setvbuf(stdout, nullptr, _IONBF, 0);
}
inline void restore_raw(const termios& old_t) {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
    setvbuf(stdout, nullptr, _IOLBF, 0);
}

inline void term_size(int& rows, int& cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        { rows = ws.ws_row; cols = ws.ws_col; }
    else { rows = 24; cols = 80; }
}
inline int term_rows() { int r,c; term_size(r,c); return r; }
inline int term_cols() { int r,c; term_size(r,c); return c; }

inline void clear()       { write(STDOUT_FILENO, "\033[2J\033[3J\033[H", 11); }
inline void hide_cursor() { write(STDOUT_FILENO, "\033[?25l", 6); }
inline void show_cursor() { write(STDOUT_FILENO, "\033[?25h", 6); }

inline int read_key() {
    int c = getchar();
    if (c == 27) {
        int c2 = getchar();
        if (c2 == '[') { int c3 = getchar();
            if (c3=='A') return 1000; if (c3=='B') return 1001;
            if (c3=='C') return 1002; if (c3=='D') return 1003; }
        return 27;
    }
    return c;
}

inline void print_divider(char c='-') {
    int w = std::min(term_cols(), 60);
    for (int i=0;i<w;i++) putchar(c); putchar('\n');
}
inline void print_item(const std::string& s, bool sel) {
    if (sel) printf("\033[7m\033[1m  %s  \033[0m\n", s.c_str());
    else     printf("  %s\n", s.c_str());
}

// 뷰포트: 헤더/푸터 제외한 표시 가능 항목 수
inline int viewport_size(int hdr, int ftr) {
    return std::max(term_rows() - hdr - ftr - 1, 3);
}
// cur가 뷰 안에 있도록 offset 조정
inline void adjust_offset(int cur, int total, int vsz, int& offset) {
    if (cur < offset) offset = cur;
    if (cur >= offset + vsz) offset = cur - vsz + 1;
    offset = std::max(0, std::min(offset, std::max(0, total - vsz)));
}

inline std::string human_size(int64_t b) {
    if (b<1024) return std::to_string(b)+" B";
    if (b<1024*1024) return std::to_string(b/1024)+" KB";
    if (b<1024*1024*1024LL) return std::to_string(b/(1024*1024))+" MB";
    return std::to_string(b/(1024*1024*1024LL))+" GB";
}

struct DirEntry { std::string name; bool is_dir; int64_t size; };
inline std::vector<DirEntry> read_dir(const std::string& path) {
    std::vector<DirEntry> v;
    DIR* d = opendir(path.c_str()); if (!d) return v;
    struct dirent* ent;
    while ((ent=readdir(d))!=nullptr) {
        if (!strcmp(ent->d_name,".") || !strcmp(ent->d_name,"..")) continue;
        std::string full = path+"/"+ent->d_name;
        struct stat st{}; stat(full.c_str(),&st);
        v.push_back({ent->d_name,(bool)S_ISDIR(st.st_mode),st.st_size});
        if (v.size()>=500) break;
    }
    closedir(d);
    std::sort(v.begin(),v.end(),[](const DirEntry&a,const DirEntry&b){
        if (a.is_dir!=b.is_dir) return a.is_dir>b.is_dir;
        return a.name<b.name; });
    return v;
}
inline std::string normalize(const std::string& p) {
    char buf[4096]{}; if (realpath(p.c_str(),buf)) return buf; return p;
}
inline std::string parent(const std::string& p) {
    if (p=="/") return "/";
    size_t sl=p.rfind('/'); if (sl==0) return "/";
    if (sl!=std::string::npos) return p.substr(0,sl); return p;
}

} // namespace tui_detail


// ─────────────────────────────────────────────────────────────────
//  tui_menu
// ─────────────────────────────────────────────────────────────────
inline int tui_menu(const std::string& title,
                    const std::vector<std::string>& items)
{
    if (items.empty()) return -1;
    int cur=0, offset=0, n=(int)items.size();
    const int HDR=4, FTR=2;
    termios old_t; tui_detail::set_raw(old_t); tui_detail::hide_cursor();

    while (true) {
        int vsz = tui_detail::viewport_size(HDR, FTR);
        tui_detail::adjust_offset(cur, n, vsz, offset);
        tui_detail::clear();
        tui_detail::print_divider('=');
        printf("  %s\n", title.c_str());
        tui_detail::print_divider('=');

        if (offset>0) printf("  \033[90m▲ %d개 더\033[0m\n", offset);
        int end = std::min(offset+vsz, n);
        for (int i=offset; i<end; i++)
            tui_detail::print_item(items[i], i==cur);
        if (end<n) printf("  \033[90m▼ %d개 더\033[0m\n", n-end);

        tui_detail::print_divider('-');
        printf("  [↑↓] 이동   [Enter] 선택   [ESC] 취소\n");
        fflush(stdout);

        int k = tui_detail::read_key();
        if      (k==1000) cur=(cur-1+n)%n;
        else if (k==1001) cur=(cur+1)%n;
        else if (k=='\n'||k=='\r') break;
        else if (k==27||k=='q') { cur=-1; break; }
    }
    tui_detail::show_cursor(); tui_detail::restore_raw(old_t);
    return cur;
}


// ─────────────────────────────────────────────────────────────────
//  tui_browse_file
// ─────────────────────────────────────────────────────────────────
inline std::string tui_browse_file(const std::string& start_path)
{
    std::string cur_path = tui_detail::normalize(start_path);
    int sel=0, offset=0;
    const int HDR=5, FTR=2;
    termios old_t; tui_detail::set_raw(old_t); tui_detail::hide_cursor();

    while (true) {
        auto entries = tui_detail::read_dir(cur_path);
        int total = (int)entries.size()+1;
        int vsz = tui_detail::viewport_size(HDR, FTR);
        tui_detail::adjust_offset(sel, total, vsz, offset);

        tui_detail::clear();
        tui_detail::print_divider('=');
        printf("  파일 선택\n");
        printf("  위치: %s\n", cur_path.c_str());
        tui_detail::print_divider('=');

        if (offset>0) printf("  \033[90m▲ %d개 더\033[0m\n", offset);
        int end = std::min(offset+vsz, total);
        for (int i=offset; i<end; i++) {
            std::string label;
            if (i==0) label="[..] 상위 폴더";
            else {
                const auto& e=entries[i-1];
                label=(e.is_dir?"[폴더] ":"[파일] ")+e.name;
                if (!e.is_dir) label+="  ("+tui_detail::human_size(e.size)+")";
            }
            tui_detail::print_item(label, i==sel);
        }
        if (end<total) printf("  \033[90m▼ %d개 더\033[0m\n", total-end);

        tui_detail::print_divider('-');
        printf("  [↑↓] 이동   [Enter] 선택/진입   [ESC] 취소\n");
        fflush(stdout);

        int k = tui_detail::read_key();
        if (k==1000) { sel=(sel-1+total)%total; }
        else if (k==1001) { sel=(sel+1)%total; }
        else if (k=='\n'||k=='\r') {
            if (sel==0) {
                std::string p=tui_detail::parent(cur_path);
                if (p!=cur_path) { cur_path=p; sel=0; offset=0; }
            } else {
                const auto& e=entries[sel-1];
                if (e.is_dir) { cur_path=cur_path+"/"+e.name; sel=0; offset=0; }
                else {
                    std::string full=cur_path+"/"+e.name;
                    tui_detail::show_cursor(); tui_detail::restore_raw(old_t);
                    int confirm = tui_menu(
                        e.name+"  ("+tui_detail::human_size(e.size)+")\n  서버에 저장하시겠습니까?",
                        {"아니오 (취소)","예 (서버에 저장)"});
                    if (confirm==1) { tui_detail::show_cursor(); return full; }
                    tui_detail::set_raw(old_t); tui_detail::hide_cursor();
                }
            }
        } else if (k==27||k=='q') break;
    }
    tui_detail::show_cursor(); tui_detail::restore_raw(old_t);
    return "";
}


// ─────────────────────────────────────────────────────────────────
//  tui_browse_dir
// ─────────────────────────────────────────────────────────────────
inline std::string tui_browse_dir(const std::string& start_path)
{
    std::string cur_path = tui_detail::normalize(start_path);
    int sel=0, offset=0;
    const int HDR=5, FTR=2;
    termios old_t; tui_detail::set_raw(old_t); tui_detail::hide_cursor();

    while (true) {
        auto entries = tui_detail::read_dir(cur_path);
        std::vector<tui_detail::DirEntry> dirs;
        for (auto& e:entries) if (e.is_dir) dirs.push_back(e);
        int total = (int)dirs.size()+2;
        int vsz = tui_detail::viewport_size(HDR, FTR);
        tui_detail::adjust_offset(sel, total, vsz, offset);

        tui_detail::clear();
        tui_detail::print_divider('=');
        printf("  저장 폴더 선택\n");
        printf("  현재: %s\n", cur_path.c_str());
        tui_detail::print_divider('=');

        if (offset>0) printf("  \033[90m▲ %d개 더\033[0m\n", offset);
        int end = std::min(offset+vsz, total);
        for (int i=offset; i<end; i++) {
            std::string label;
            if (i==0)      label="[✓] 현재 폴더에 저장";
            else if (i==1) label="[..] 상위 폴더";
            else           label="[폴더] "+dirs[i-2].name;
            tui_detail::print_item(label, i==sel);
        }
        if (end<total) printf("  \033[90m▼ %d개 더\033[0m\n", total-end);

        tui_detail::print_divider('-');
        printf("  [↑↓] 이동   [Enter] 확정/진입   [ESC] 취소\n");
        fflush(stdout);

        int k = tui_detail::read_key();
        if (k==1000) { sel=(sel-1+total)%total; }
        else if (k==1001) { sel=(sel+1)%total; }
        else if (k=='\n'||k=='\r') {
            if (sel==0) break;
            else if (sel==1) {
                std::string p=tui_detail::parent(cur_path);
                if (p!=cur_path) { cur_path=p; sel=0; offset=0; }
            } else { cur_path=cur_path+"/"+dirs[sel-2].name; sel=0; offset=0; }
        } else if (k==27||k=='q') {
            tui_detail::show_cursor(); tui_detail::restore_raw(old_t); return "";
        }
    }
    tui_detail::show_cursor(); tui_detail::restore_raw(old_t);
    return cur_path;
}


// ─────────────────────────────────────────────────────────────────
//  tui_select_cloud_file
// ─────────────────────────────────────────────────────────────────
#include <nlohmann/json.hpp>

inline int tui_select_cloud_file(const std::string& title,
                                  const nlohmann::json& files,
                                  int64_t used=0, int64_t total_bytes=0)
{
    if (files.empty()) return -1;
    int cur=0, offset=0, n=(int)files.size();
    const int HDR=(total_bytes>0?5:4), FTR=2;
    termios old_t; tui_detail::set_raw(old_t); tui_detail::hide_cursor();

    while (true) {
        int vsz = tui_detail::viewport_size(HDR, FTR);
        tui_detail::adjust_offset(cur, n, vsz, offset);

        tui_detail::clear();
        tui_detail::print_divider('=');
        printf("  %s\n", title.c_str());
        if (total_bytes>0)
            printf("  사용: %s / 전체: %s\n",
                   tui_detail::human_size(used).c_str(),
                   tui_detail::human_size(total_bytes).c_str());
        tui_detail::print_divider('=');

        if (offset>0) printf("  \033[90m▲ %d개 더\033[0m\n", offset);
        int end = std::min(offset+vsz, n);
        for (int i=offset; i<end; i++) {
            const auto& f=files[i];
            std::string name  = f.value("file_name","");
            std::string fsize = tui_detail::human_size(f.value("file_size",(int64_t)0));
            std::string date  = f.value("created_at","");
            std::string fol   = f.value("folder","");
            if (date.size()>10) date=date.substr(0,10);
            std::string label = name+"  "+fsize+"  "+date;
            if (!fol.empty()) label+="  /"+fol;
            tui_detail::print_item(label, i==cur);
        }
        if (end<n) printf("  \033[90m▼ %d개 더\033[0m\n", n-end);

        tui_detail::print_divider('-');
        printf("  [↑↓] 이동   [Enter] 선택   [ESC] 취소\n");
        fflush(stdout);

        int k = tui_detail::read_key();
        if      (k==1000) cur=(cur-1+n)%n;
        else if (k==1001) cur=(cur+1)%n;
        else if (k=='\n'||k=='\r') break;
        else if (k==27||k=='q') { cur=-1; break; }
    }
    tui_detail::show_cursor(); tui_detail::restore_raw(old_t);
    return cur;
}
