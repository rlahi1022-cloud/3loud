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
#include <atomic>
#include <sys/select.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

namespace tui_detail
{

    inline void set_raw(termios &old_t)
    {
        termios t;
        tcgetattr(STDIN_FILENO, &t);
        old_t = t;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        setvbuf(stdout, nullptr, _IONBF, 0);
    }
    inline void restore_raw(const termios &old_t)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
        setvbuf(stdout, nullptr, _IOLBF, 0);
    }

    inline void term_size(int &rows, int &cols)
    {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        {
            rows = ws.ws_row;
            cols = ws.ws_col;
        }
        else
        {
            rows = 24;
            cols = 80;
        }
    }
    inline int term_rows()
    {
        int r, c;
        term_size(r, c);
        return r;
    }
    inline int term_cols()
    {
        int r, c;
        term_size(r, c);
        return c;
    }

    inline void clear() { write(STDOUT_FILENO, "\033[2J\033[3J\033[H", 11); }
    inline void hide_cursor() { write(STDOUT_FILENO, "\033[?25l", 6); }
    inline void show_cursor() { write(STDOUT_FILENO, "\033[?25h", 6); }

// 1바이트 읽기 (select 후 read() 직접 사용 - getchar() 버퍼링 문제 회피)
inline int read_one(int timeout_us) {
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    struct timeval tv{ timeout_us / 1000000, timeout_us % 1000000 };
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
        return -2;
    unsigned char c;
    if (::read(STDIN_FILENO, &c, 1) != 1) return -2;
    return (int)c;
}

// 논블로킹 키 읽기: timeout_ms 동안 입력 대기, 없으면 -2 반환
inline int read_key_timeout(int timeout_ms) {
    int c = read_one(timeout_ms * 1000);
    if (c == -2) return -2;  // 타임아웃

    if (c == 27) {
        // ESC 시퀀스 확인 (50ms 대기)
        int c2 = read_one(50000);
        if (c2 == -2) return 27;  // 단독 ESC
        if (c2 == '[') {
            int c3 = read_one(50000);
            if (c3 == -2) return 27;
            if (c3=='A') return 1000;
            if (c3=='B') return 1001;
            if (c3=='C') return 1002;
            if (c3=='D') return 1003;
        }
        return 27;
    }
    return c;
}

inline int read_key() {
    // 블로킹: 최대 10초 대기 (사실상 무한)
    return read_key_timeout(10000);
}

    inline void print_divider(char c = '-')
    {
        int w = std::min(term_cols(), 60);
        for (int i = 0; i < w; i++)
        {
            putchar(c);
        }
        putchar('\n');
    }
    inline void print_item(const std::string &s, bool sel)
    {
        if (sel)
            printf("\033[7m\033[1m  %s  \033[0m\n", s.c_str());
        else
            printf("  %s\n", s.c_str());
    }

    // 뷰포트: 헤더/푸터 제외한 표시 가능 항목 수
    inline int viewport_size(int hdr, int ftr)
    {
        return std::max(term_rows() - hdr - ftr - 1, 3);
    }
    // cur가 뷰 안에 있도록 offset 조정
    inline void adjust_offset(int cur, int total, int vsz, int &offset)
    {
        if (cur < offset)
            offset = cur;
        if (cur >= offset + vsz)
            offset = cur - vsz + 1;
        offset = std::max(0, std::min(offset, std::max(0, total - vsz)));
    }

    inline std::string human_size(int64_t b)
    {
        if (b < 1024)
            return std::to_string(b) + " B";
        if (b < 1024 * 1024)
            return std::to_string(b / 1024) + " KB";
        if (b < 1024 * 1024 * 1024LL)
            return std::to_string(b / (1024 * 1024)) + " MB";
        return std::to_string(b / (1024 * 1024 * 1024LL)) + " GB";
    }

    struct DirEntry
    {
        std::string name;
        bool is_dir;
        int64_t size;
    };
    inline std::vector<DirEntry> read_dir(const std::string &path)
    {
        std::vector<DirEntry> v;
        DIR *d = opendir(path.c_str());
        if (!d)
            return v;
        struct dirent *ent;
        while ((ent = readdir(d)) != nullptr)
        {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
                continue;
            std::string full = path + "/" + ent->d_name;
            struct stat st{};
            stat(full.c_str(), &st);
            v.push_back({ent->d_name, (bool)S_ISDIR(st.st_mode), st.st_size});
            if (v.size() >= 500)
                break;
        }
        closedir(d);
        std::sort(v.begin(), v.end(), [](const DirEntry &a, const DirEntry &b)
                  {
        if (a.is_dir!=b.is_dir) return a.is_dir>b.is_dir;
        return a.name<b.name; });
        return v;
    }
    inline std::string normalize(const std::string &p)
    {
        char buf[4096]{};
        if (realpath(p.c_str(), buf))
            return buf;
        return p;
    }
    inline std::string parent(const std::string &p)
    {
        if (p == "/")
            return "/";
        size_t sl = p.rfind('/');
        if (sl == 0)
            return "/";
        if (sl != std::string::npos)
            return p.substr(0, sl);
        return p;
    }

} // namespace tui_detail

// ─────────────────────────────────────────────────────────────────
//  tui_menu
//  items_fn: nullptr 이면 items 고정 사용
//            함수 포인터가 있으면 매 루프마다 호출해 항목 동적 갱신
//            (실시간 [!] 뱃지 갱신에 사용)
// ─────────────────────────────────────────────────────────────────
#include <functional>
// 파일 전송 상태 공유 변수 (file_client.cpp에서 정의)
// tui_menu가 footer에 전송 진행률을 표시하기 위해 참조
extern std::atomic<bool>   g_file_transfer_in_progress;
extern std::atomic<int>    g_upload_progress_pct;   // 0~100, 업로드 스레드가 갱신
extern std::atomic<int>    g_upload_progress_cur;   // 현재 청크
extern std::atomic<int>    g_upload_progress_tot;   // 전체 청크
extern std::atomic<bool>   g_download_in_progress;  // 다운로드 진행 중 플래그
extern std::atomic<int>    g_download_progress_pct; // 0~100, 다운로드 스레드가 갱신
extern std::atomic<int>    g_download_progress_cur;
extern std::atomic<int>    g_download_progress_tot;

inline int tui_menu(const std::string& title,
                    const std::vector<std::string>& items,
                    std::function<std::vector<std::string>()> items_fn = nullptr)
{
    if (items.empty() && !items_fn) return -1;
    std::vector<std::string> cur_items = items;
    int cur=0, offset=0;
    int n=(int)cur_items.size();
    const int HDR=4, FTR=3;  // footer 1줄 늘림 (전송 상태 표시용)
    termios old_t; tui_detail::set_raw(old_t); tui_detail::hide_cursor();

    bool need_redraw = true;
    bool last_transfer  = false;
    bool last_download  = false;
    int  last_pct       = -1;
    int  last_dl_pct    = -1;

    while (true) {
        // 동적 항목 갱신
        if (items_fn) {
            auto new_items = items_fn();
            if (new_items != cur_items) {
                cur_items  = new_items;
                n          = (int)cur_items.size();
                cur        = std::min(cur, n - 1);
                need_redraw = true;
            }
        }

        // 전송/다운로드 상태 변화 감지 → 변화 있으면 footer 갱신
        bool cur_transfer  = g_file_transfer_in_progress.load();
        bool cur_download  = g_download_in_progress.load();
        int  cur_pct       = g_upload_progress_pct.load();
        int  cur_dl_pct    = g_download_progress_pct.load();
        if (cur_transfer != last_transfer || cur_download != last_download ||
            cur_pct != last_pct || cur_dl_pct != last_dl_pct) {
            last_transfer = cur_transfer;
            last_download = cur_download;
            last_pct      = cur_pct;
            last_dl_pct   = cur_dl_pct;
            need_redraw   = true;
        }

        if (need_redraw) {
            int vsz = tui_detail::viewport_size(HDR, FTR);
            tui_detail::adjust_offset(cur, n, vsz, offset);
            tui_detail::clear();
            tui_detail::print_divider('=');
            printf("  %s\n", title.c_str());
            tui_detail::print_divider('=');

            if (offset>0) printf("  \033[90m▲ %d개 더\033[0m\n", offset);
            int end = std::min(offset+vsz, n);
            for (int i=offset; i<end; i++)
                tui_detail::print_item(cur_items[i], i==cur);
            if (end<n) printf("  \033[90m▼ %d개 더\033[0m\n", n-end);

            tui_detail::print_divider('-');
            printf("  [↑↓] 이동   [Enter] 선택   [ESC] 취소\n");

            // ── 전송 상태 footer (업로드 / 다운로드 각각 표시) ──
            bool uploading   = g_file_transfer_in_progress.load();
            bool downloading = g_download_in_progress.load();

            auto make_bar = [](int pct) -> std::string {
                int fill = pct / 5;
                std::string bar = "[";
                for (int b = 0; b < 20; b++) bar += (b < fill) ? '#' : '.';
                bar += "]";
                return bar;
            };

            if (uploading && !downloading) {
                int pct   = g_upload_progress_pct.load();
                int cur_c = g_upload_progress_cur.load();
                int tot_c = g_upload_progress_tot.load();
                printf("  \033[33m[파일 저장 중]\033[0m %s %d%% (%d/%d)\n",
                       make_bar(pct).c_str(), pct, cur_c, tot_c);
            } else if (downloading && !uploading) {
                int pct   = g_download_progress_pct.load();
                int cur_c = g_download_progress_cur.load();
                int tot_c = g_download_progress_tot.load();
                if (pct >= 100)
                    printf("  \033[32m[파일 수신 완료]\033[0m\n");
                else
                    printf("  \033[36m[파일 수신 중]\033[0m  %s %d%% (%d/%d)\n",
                           make_bar(pct).c_str(), pct, cur_c, tot_c);
            } else if (uploading && downloading) {
                // 업로드·다운로드 동시 표시 (불가 케이스지만 방어)
                printf("  \033[33m[업로드]\033[0m %d%%  \033[36m[다운로드]\033[0m %d%%\n",
                       g_upload_progress_pct.load(), g_download_progress_pct.load());
            } else {
                printf("\n");  // 빈 줄로 footer 높이 유지
            }

            fflush(stdout);
            need_redraw = false;
        }

        // 전송/다운로드 중이면 100ms 주기로 footer 갱신, 아니면 500ms
        bool any_transfer = g_file_transfer_in_progress.load() || g_download_in_progress.load();
        int k = (items_fn || any_transfer)
            ? tui_detail::read_key_timeout(100)
            : tui_detail::read_key_timeout(500);

        if (k == -2) {
            continue;
        }

        need_redraw = true;
        if      (k==1000) cur=(cur-1+n)%n;
        else if (k==1001) cur=(cur+1)%n;
        else if (k=='\n'||k=='\r') break;
        else if (k==27||k=='q') { cur=-1; break; }
    }
    tui_detail::show_cursor();
    tui_detail::restore_raw(old_t);
    return cur;
}

// ─────────────────────────────────────────────────────────────────
//  tui_browse_file
// ─────────────────────────────────────────────────────────────────
inline std::string tui_browse_file(const std::string &start_path)
{
    std::string cur_path = tui_detail::normalize(start_path);
    int sel = 0, offset = 0;
    const int HDR = 5, FTR = 2;
    termios old_t;
    tui_detail::set_raw(old_t);
    tui_detail::hide_cursor();

    while (true)
    {
        auto entries = tui_detail::read_dir(cur_path);
        int total = (int)entries.size() + 1;
        int vsz = tui_detail::viewport_size(HDR, FTR);
        tui_detail::adjust_offset(sel, total, vsz, offset);

        tui_detail::clear();
        tui_detail::print_divider('=');
        printf("  파일 선택\n");
        printf("  위치: %s\n", cur_path.c_str());
        tui_detail::print_divider('=');

        if (offset > 0)
            printf("  \033[90m▲ %d개 더\033[0m\n", offset);
        int end = std::min(offset + vsz, total);
        for (int i = offset; i < end; i++)
        {
            std::string label;
            if (i == 0)
                label = "[..] 상위 폴더";
            else
            {
                const auto &e = entries[i - 1];
                label = (e.is_dir ? "[폴더] " : "[파일] ") + e.name;
                if (!e.is_dir)
                    label += "  (" + tui_detail::human_size(e.size) + ")";
            }
            tui_detail::print_item(label, i == sel);
        }
        if (end < total)
            printf("  \033[90m▼ %d개 더\033[0m\n", total - end);

        tui_detail::print_divider('-');
        printf("  [↑↓] 이동   [Enter] 선택/진입   [ESC] 취소\n");
        fflush(stdout);

        int k = tui_detail::read_key();
        if (k == 1000)
        {
            sel = (sel - 1 + total) % total;
        }
        else if (k == 1001)
        {
            sel = (sel + 1) % total;
        }
        else if (k == '\n' || k == '\r')
        {
            if (sel == 0)
            {
                std::string p = tui_detail::parent(cur_path);
                if (p != cur_path)
                {
                    cur_path = p;
                    sel = 0;
                    offset = 0;
                }
            }
            else
            {
                const auto &e = entries[sel - 1];
                if (e.is_dir)
                {
                    cur_path = cur_path + "/" + e.name;
                    sel = 0;
                    offset = 0;
                }
                else
                {
                    std::string full = cur_path + "/" + e.name;
                    tui_detail::show_cursor();
                    tui_detail::restore_raw(old_t);
                    int confirm = tui_menu(
                        e.name + "  (" + tui_detail::human_size(e.size) + ")\n  서버에 저장하시겠습니까?",
                        {"아니오 (취소)", "예 (서버에 저장)"});
                    if (confirm == 1)
                    {
                        tui_detail::show_cursor();
                        return full;
                    }
                    tui_detail::set_raw(old_t);
                    tui_detail::hide_cursor();
                }
            }
        }
        else if (k == 27 || k == 'q')
            break;
    }
    tui_detail::show_cursor();
    tui_detail::restore_raw(old_t);
    return "";
}

// ─────────────────────────────────────────────────────────────────
//  tui_browse_dir
// ─────────────────────────────────────────────────────────────────
inline std::string tui_browse_dir(const std::string &start_path)
{
    std::string cur_path = tui_detail::normalize(start_path);
    int sel = 0, offset = 0;
    const int HDR = 5, FTR = 2;
    termios old_t;
    tui_detail::set_raw(old_t);
    tui_detail::hide_cursor();

    while (true)
    {
        auto entries = tui_detail::read_dir(cur_path);
        std::vector<tui_detail::DirEntry> dirs;
        for (auto &e : entries)
            if (e.is_dir)
                dirs.push_back(e);
        int total = (int)dirs.size() + 2;
        int vsz = tui_detail::viewport_size(HDR, FTR);
        tui_detail::adjust_offset(sel, total, vsz, offset);

        tui_detail::clear();
        tui_detail::print_divider('=');
        printf("  저장 폴더 선택\n");
        printf("  현재: %s\n", cur_path.c_str());
        tui_detail::print_divider('=');

        if (offset > 0)
            printf("  \033[90m▲ %d개 더\033[0m\n", offset);
        int end = std::min(offset + vsz, total);
        for (int i = offset; i < end; i++)
        {
            std::string label;
            if (i == 0)
                label = "[✓] 현재 폴더에 저장";
            else if (i == 1)
                label = "[..] 상위 폴더";
            else
                label = "[폴더] " + dirs[i - 2].name;
            tui_detail::print_item(label, i == sel);
        }
        if (end < total)
            printf("  \033[90m▼ %d개 더\033[0m\n", total - end);

        tui_detail::print_divider('-');
        printf("  [↑↓] 이동   [Enter] 확정/진입   [ESC] 취소\n");
        fflush(stdout);

        int k = tui_detail::read_key();
        if (k == 1000)
        {
            sel = (sel - 1 + total) % total;
        }
        else if (k == 1001)
        {
            sel = (sel + 1) % total;
        }
        else if (k == '\n' || k == '\r')
        {
            if (sel == 0)
                break;
            else if (sel == 1)
            {
                std::string p = tui_detail::parent(cur_path);
                if (p != cur_path)
                {
                    cur_path = p;
                    sel = 0;
                    offset = 0;
                }
            }
            else
            {
                cur_path = cur_path + "/" + dirs[sel - 2].name;
                sel = 0;
                offset = 0;
            }
        }
        else if (k == 27 || k == 'q')
        {
            tui_detail::show_cursor();
            tui_detail::restore_raw(old_t);
            return "";
        }
    }
    tui_detail::show_cursor();
    tui_detail::restore_raw(old_t);
    return cur_path;
}

// ─────────────────────────────────────────────────────────────────
//  tui_select_cloud_file
// ─────────────────────────────────────────────────────────────────
#include <nlohmann/json.hpp>

inline int tui_select_cloud_file(const std::string &title,
                                 const nlohmann::json &files,
                                 int64_t used = 0, int64_t total_bytes = 0)
{
    if (files.empty())
        return -1;
    int cur = 0, offset = 0, n = (int)files.size();
    const int HDR = (total_bytes > 0 ? 5 : 4), FTR = 2;
    termios old_t;
    tui_detail::set_raw(old_t);
    tui_detail::hide_cursor();

    while (true)
    {
        int vsz = tui_detail::viewport_size(HDR, FTR);
        tui_detail::adjust_offset(cur, n, vsz, offset);

        tui_detail::clear();
        tui_detail::print_divider('=');
        printf("  %s\n", title.c_str());
        if (total_bytes > 0)
            printf("  사용: %s / 전체: %s\n",
                   tui_detail::human_size(used).c_str(),
                   tui_detail::human_size(total_bytes).c_str());
        tui_detail::print_divider('=');

        if (offset > 0)
            printf("  \033[90m▲ %d개 더\033[0m\n", offset);
        int end = std::min(offset + vsz, n);
        for (int i = offset; i < end; i++)
        {
            const auto &f = files[i];
            std::string name = f.value("file_name", "");
            std::string fsize = tui_detail::human_size(f.value("file_size", (int64_t)0));
            std::string date = f.value("created_at", "");
            std::string fol = f.value("folder", "");
            if (date.size() > 10)
                date = date.substr(0, 10);
            std::string label = name + "  " + fsize + "  " + date;
            if (!fol.empty())
                label += "  /" + fol;
            tui_detail::print_item(label, i == cur);
        }
        if (end < n)
            printf("  \033[90m▼ %d개 더\033[0m\n", n - end);

        tui_detail::print_divider('-');
        printf("  [↑↓] 이동   [Enter] 선택   [ESC] 취소\n");
        fflush(stdout);

        int k = tui_detail::read_key();
        if (k == 1000)
            cur = (cur - 1 + n) % n;
        else if (k == 1001)
            cur = (cur + 1) % n;
        else if (k == '\n' || k == '\r')
            break;
        else if (k == 27 || k == 'q')
        {
            cur = -1;
            break;
        }
    }
    tui_detail::show_cursor();
    tui_detail::restore_raw(old_t);
    return cur;
}
