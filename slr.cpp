#include <conio.h>
#include <ctype.h>
#include <dir.h>
#include <dos.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#include <mem.h>
#include <share.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEAD_CHARS 4
#define ANI_FRAME_LENGTH 12
#define PROGRESS_BAR_SIZE 6

const char* option_scan_root = NULL;
const char* option_file_filter = "*.bat\x0*.com\x0*.exe\x0";
bool option_noexec = false;

text_info ti;

char* render_buffer = NULL;
int render_buffer_size = 0;
char line[128];
int line_length = 0;
int edit_offset = 0;

struct item
{
    char* buf;

    const char* path() const
    {
        return buf;
    }

    const char* name() const
    {
        return path() + strlen(path()) + 1;
    }
};

item* list = NULL;
int list_size = 0;
int list_capacity = 0;
int list_offset = 0;
int list_height = 0;
int selected_item = -1;

bool confirmed_action = false;

unsigned skipped_dirs = 0, skipped_files = 0;

struct
{
    bool list;
} invalid = {};

class char_buffer
{
public:
    char_buffer() : buf(NULL)
    {
    }

    char_buffer(size_t size) : buf((char*)malloc(size))
    {
    }

    char_buffer(char_buffer& other) : buf(other.buf)
    {
        other.buf = NULL;
    }

    ~char_buffer()
    {
        free(buf);
    }

    operator char*() const
    {
        return buf;
    }

    char* release()
    {
        char* ret = buf;
        buf = NULL;
        return ret;
    }

    bool resize(size_t size)
    {
        void* new_buf = realloc(buf, size);
        if(!new_buf) return false;
        buf = (char*)new_buf;
        return true;
    }

    void grab(char_buffer& other)
    {
        free(buf);
        buf = other.buf;
        other.buf = NULL;
    }

private:
    char* buf;
};

char* get_render_buffer(int dim)
{
    int size = 2 * dim;
    if(size > render_buffer_size)
    {
        void* buf = realloc(render_buffer, size);
        if(!buf) return NULL;
        render_buffer = (char*)buf;
        render_buffer_size = size;
    }
    return render_buffer;
}

void render_fill(int x, int y, int w, int h, char c, char attr)
{
    int dim = w * h;
    char* buffer = get_render_buffer(dim);
    if(!buffer) return;

    for(char* p = buffer; dim > 0; --dim)
    {
        *p++ = c;
        *p++ = attr;
    }
    puttext(x, y, x + w - 1, y + h - 1, buffer);
}

void render_str(int x, int y, const char* str)
{
    char* buffer = get_render_buffer(ti.screenwidth);
    if(!buffer) return;

    int len = 0;
    for(char* p = buffer; *str && len < ti.screenwidth; ++len)
    {
        *p++ = *str++;
        *p++ = ti.normattr;
    }
    puttext(x, y, x + len - 1, y, buffer);
}

void render_top_line()
{
    render_fill(1, 1, ti.screenwidth, 1, '-', ti.normattr);
    if(skipped_dirs || skipped_files)
    {
        char_buffer s(128);
        if(s)
        {
            strcpy(s, " Skipped ");
            int start = strlen(s);
            if(skipped_dirs)
            {
                sprintf(s + start, "%u dirs", skipped_dirs);
            }
            if(skipped_files)
            {
                int len = strlen(s);
                sprintf(s + len, "%s%u files", (len == start) ? "" : ", ", skipped_files);
            }
            strcat(s, " ");
            render_str(1 + ti.screenwidth - strlen(s), 1, s);
        }
    }
}

void render_bottom_line()
{
    render_fill(1, ti.screenheight - 1, ti.screenwidth, 1, '-', ti.normattr);
}

void render_edit()
{
    char* buffer = get_render_buffer(ti.screenwidth);
    if(!buffer) return;

    char* pline = line + edit_offset;
    char* pedit = buffer;
    for(int i = 0; i < ti.screenwidth; ++i)
    {
        *pedit++ = (pline < line + line_length) ? *pline++ : ' ';
        *pedit++ = ti.normattr;
    }
    int y = wherey();
    puttext(1, y, ti.screenwidth, y, buffer);
}

void render_progress(int frame)
{
    if(frame < 0 || frame >= 2 * PROGRESS_BAR_SIZE) return;

    char_buffer s(PROGRESS_BAR_SIZE + sizeof(" [] Scanning... "));
    if(!s)
        return;

    const bar_ofs = 2;
    const char head_r = 0x07, tail_r = 0xF9;
    const char head_l = 0x07, tail_l = 0xF9;

    sprintf(s, " [%*s] Scanning... ", PROGRESS_BAR_SIZE, "");
    int pos = frame % PROGRESS_BAR_SIZE;
    if(frame < PROGRESS_BAR_SIZE)
    {
        s[bar_ofs + pos] = head_r;
        if(pos > 0) s[bar_ofs + pos - 1] = tail_r;
    }
    else
    {
        s[bar_ofs + PROGRESS_BAR_SIZE - 1 - pos] = head_l;
        if(pos > 0) s[bar_ofs + PROGRESS_BAR_SIZE - pos] = tail_l;
    }
    render_str(1 + ti.screenwidth - strlen(s), 1, s);
}

const char* next_token(const char* s, const char* end)
{
    while(s != end && *s == ' ') ++s;
    return (s != end) ? s : NULL;
}

int token_length(const char* token, const char* end)
{
    const char* s = token;
    while(s != end && *s != ' ') ++s;
    return (int)(s - token);
}

class tokenizer
{
public:
    tokenizer() : t(line), end(line + line_length), len(0)
    {
    }

    const char* token()
    {
        t = next_token(t + len, end);
        len = token_length(t, end);
        return t;
    }

    int length() const
    {
        return len;
    }

private:
    const char* t;
    const char* const end;
    int len;
};

const char* find_match(const char* input, const char* search, int search_len)
{
    int slen = strlen(input);
    for(const char* s = input; search_len <= slen; ++s, --slen)
    {
        if(!strnicmp(s, search, search_len))
            return s;
    }
    return NULL;
}

bool match_item(const item& it)
{
    tokenizer t;
    const char* p = t.token();
    if(!p) return true;
    const char* name = it.name();
    do
    {
        if(!find_match(name, p, t.length()))
            return false;
    }
    while((p = t.token()) != NULL);
    return true;
}

int prev_match_item(int i)
{
    while(i > 0)
    {
        if(match_item(list[--i]))
        return i;
    }
    return -1;
}

int next_match_item(int i)
{
    for(; i < list_size; ++i)
    {
        if(match_item(list[i]))
            return i;
    }
    return -1;
}

void render_list()
{
    char* buffer = get_render_buffer(ti.screenwidth);
    if(!buffer) return;

    if(selected_item >= 0 && !match_item(list[selected_item]))
        selected_item = -1;

    int y = 2;
    for(int i = list_offset; i < list_size; ++i)
    {
        if(y >= 2 + list_height) break;

        const item& it = list[i];
        if(!match_item(it))
            continue;

        if(selected_item < 0) selected_item = i;

        const char* const name = it.name();

        const char* pname = name;
        char* pbuf = buffer;
        char attr = (i == selected_item) ? (LIGHTGRAY << 4) : ti.normattr;
        for(int n = 0; n < ti.screenwidth; ++n)
        {
            *pbuf++ = (n != 0 && *pname) ? *pname++ : ' ';
            *pbuf++ = attr;
        }

        tokenizer t;
        while(const char* p = t.token())
        {
            if(const char* m = find_match(name, p, t.length()))
            {
                int start = 1 + (int)(m - name);
                for(int i = 0; i < t.length(); ++i)
                    buffer[2 * (start + i) + 1] |= WHITE;
            }
        }

        puttext(1, y, ti.screenwidth, y, buffer);
        ++y;
    }

    for(; y < 2 + list_height; ++y)
        render_fill(1, y, ti.screenwidth, 1, ' ', ti.normattr);

    invalid.list = false;
}

void select_item(int i)
{
    if(i < 0) i = -1;
    if(i == selected_item) return;

    if(i < selected_item)
    {
        if(selected_item >= 0 && i < list_offset) list_offset = i;
        selected_item = i;
    }
    else
    {
        int ofs = selected_item = i;
        for(int n = 0; n < list_height && ofs != list_offset && i >= 0; ++n)
            i = prev_match_item(ofs = i);

        list_offset = ofs;
    }

    invalid.list = true;
}

void select_prev_item(int start)
{
    int i = prev_match_item(start);
    if(i >= 0) select_item(i);
}

void select_next_item(int start)
{
    int i = next_match_item(start);
    if(i >= 0) select_item(i);
}

void on_line_changed()
{
    invalid.list = true;

    int i = selected_item;
    selected_item = -1;
    list_offset = 0;
    select_item(i);
}

bool process_input()
{
    int c = getch();
    if(c == 0)
    {
        c = getch();
        if(c == 75) // left
        {
            gotoxy(wherex() - 1, wherey());
        }
        else if(c == 77) // right
        {
            int x = wherex();
            if(edit_offset + x - 1 < line_length)
                gotoxy(wherex() + 1, wherey());
        }
        else if(c == 115) // ctrl-left
        {
            int x = wherex();
            int y = wherey();

            if(edit_offset + x - 1 != 0)
            {
                edit_offset = 0;
                gotoxy(1, y);
                render_edit();
            }
        }
        else if(c == 116) // ctrl-right
        {
            int x = wherex();
            int y = wherey();

            if(edit_offset + x - 1 != line_length)
            {
                edit_offset = (line_length < ti.screenwidth) ? 0 : (line_length - ti.screenwidth + 1);
                gotoxy(1 + line_length - edit_offset, y);
                render_edit();
            }
        }
        else if(c == 72) // up
        {
            select_prev_item(selected_item < 0 ? list_size : selected_item);
        }
        else if(c == 80) // down
        {
            select_next_item(selected_item < 0 ? 0 : (selected_item + 1));
        }
        else if(c == 71) // home
        {
            select_next_item(0);
        }
        else if(c == 79) // end
        {
            select_prev_item(list_size);
        }
        else if(c == 73) // pageup
        {
            int sel = (selected_item >= 0) ? selected_item : next_match_item(0);
            for(int n = 1; n < list_height && sel >= 0; ++n)
            {
                int i = prev_match_item(sel);
                if(i < 0) break;
                sel = i;
            }
            if(sel >= 0) select_item(sel);
        }
        else if(c == 81) // pagedown
        {
            int sel = (selected_item >= 0) ? selected_item : next_match_item(0);
            for(int n = 1; n < list_height && sel >= 0; ++n)
            {
                int i = next_match_item(sel + 1);
                if(i < 0) break;
                sel = i;
            }
            if(sel >= 0) select_item(sel);
        }
        else if(c == 83) // delete
        {
            int x = wherex();

            int ins = edit_offset + x - 1;
            if(ins < line_length)
            {
                memmove(line + ins, line + ins + 1, (--line_length) - ins);
                render_edit();
                on_line_changed();
            }
        }
    }
    else if(c == 3 || c == 17) // ctrl-c | ctrl-q
    {
        return false;
    }
    else if(c == 13) // enter
    {
        if(selected_item >= 0)
        {
            confirmed_action = true;
            return false;
        }
    }
    else if(c == 27) // escape
    {
        if(line_length != 0)
        {
            line_length = 0;
            gotoxy(1, wherey());
            render_edit();
            on_line_changed();
        }
    }
    else if(c == 8) // backspace
    {
        int x = wherex();
        int y = wherey();

        if(x > 1 || edit_offset > 0)
        {
            int ins = edit_offset + x - 1;
            memmove(line + ins - 1, line + ins, (line_length--) - ins);

            if(edit_offset > 0 && x == 1 + LEAD_CHARS)
            {
                --edit_offset;
            }
            else
            {
               gotoxy(x - 1, y);
            }

            render_edit();
            on_line_changed();
        }
    }
    else if(line_length < sizeof(line))
    {
        int x = wherex();
        int y = wherey();

        int ins = edit_offset + x - 1;
        memmove(line + ins + 1, line + ins, (line_length++) - ins);
        line[ins] = c;

        if(x == ti.screenwidth)
        {
            ++edit_offset;
        }
        render_edit();
        gotoxy(x + 1, y);

        on_line_changed();
    }

    return true;
}

int ani_time()
{
    struct time t;
    gettime(&t);
    return t.ti_sec * 100 + t.ti_hund;
}

int ani_time(int base, int frames)
{
    return (base + frames * ANI_FRAME_LENGTH) % 6000;
}

int ani_frames_since(int last)
{
    int t = ani_time();
    int d = (t >= last) ? (t - last) : (6000 - last + t);
    return d / ANI_FRAME_LENGTH;
}

class scanner
{
public:
    scanner() : dirs(NULL), dirs_capacity(0), dirs_size(0), ff_done(-1)
    {
        skipped_dirs = 0;
        add_dir(option_scan_root);
    }

    ~scanner()
    {
        clear_dirs();
    }

    const char* dir() const
    {
        return dirs;
    }

    const ffblk* next();

private:
    char* dirs;
    size_t dirs_capacity;
    size_t dirs_size;
    int ff_done;
    struct ffblk ffb;

    void clear_dirs()
    {
        free(dirs);
        dirs = NULL;
        dirs_capacity = 0;
        dirs_size = 0;
    }

    void add_dir(const char* path);
};

void scanner::add_dir(const char* path)
{
    int size = dirs_size + strlen(path) + 1;
    if(size > dirs_capacity)
    {
        void* buf = realloc(dirs, size);
        if(!buf)
        {
            ++skipped_dirs;
            return;
        }
        dirs = (char*)buf;
        dirs_capacity = size;
    }
    strcpy(dirs + dirs_size, path);
    dirs_size = size;
}

const ffblk* scanner::next()
{
    for(;;)
    {
        if(!ff_done)
        {
            ff_done = findnext(&ffb);
        }
        else if(dirs_size > 0)
        {
            int len = strlen(dirs);
            char_buffer path(len + 4 + 1);
            if(!path)
            {
                ++skipped_dirs;
            }
            else
            {
                strcpy(path, dirs);
                strcpy(path + len, "\\*.*");
                ff_done = findfirst(path, &ffb, FA_DIREC);
            }
        }

        if(!ff_done || dirs_size == 0) break;

        int len = strlen(dirs);
        memmove(dirs, dirs + len + 1, dirs_size -= len + 1);

        if(dirs_capacity - dirs_size >= 256)
        {
            if(void* buf = realloc(dirs, dirs_size))
            {
                dirs = (char*)buf;
                dirs_capacity = dirs_size;
            }
        }
    }

    if(ff_done)
    {
        clear_dirs();
        return NULL;
    }

    if((ffb.ff_attrib & FA_DIREC) != 0)
    {
        if(ffb.ff_name[0] != '.')
        {
            int len = strlen(dirs);
            char_buffer path(len + 1 + strlen(ffb.ff_name) + 1);
            if(!path)
            {
                ++skipped_dirs;
            }
            else
            {
                strcpy(path, dirs);
                path[len] = '\\';
                strcpy(path + len + 1, ffb.ff_name);
                add_dir(path);
            }
        }
    }

    return &ffb;
}

char_buffer getline(FILE* f)
{
    size_t size = 32;
    char_buffer ret(size);
    char* p = ret;

    char c;
    do
    {
        c = fgetc(f);
        if(c == EOF || c == '\n') c = 0;
        if(p - (const char*)ret == size)
        {
            if(!ret.resize(size + 32))
                return NULL;
            p = ret + size;
            size += 32;
        }
        *p++ = c;
    }
    while(c);

    return ret;
}

char_buffer read_item_name_bat(const char* path)
{
    char_buffer name;
    int fd = sopen(path, O_RDONLY | O_TEXT, SH_DENYNONE);
    if(fd != -1)
    {
        if(FILE* f = fdopen(fd, "rt"))
        {
            char_buffer line = getline(f);
            if(line && strlen(line) > 3 && !strnicmp(line, ":: ", 3))
            {
                memmove(line, line + 3, strlen(line + 3) + 1);
                name.grab(line);
            }
            fclose(f);
        }
        close(fd);
    }
    return name;
}

char_buffer read_item_name(const char* path)
{
    const char* dot = strrchr(path, '.');
    if(dot && !stricmp(dot + 1, "bat"))
    {
        char_buffer name = read_item_name_bat(path);
        if(name) return name;
    }

    char_buffer name(strlen(path) + 1);
    strcpy(name, path);
    return name;
}

char_buffer read_item_data(const char* dir, const char* fname)
{
    int dir_len = strlen(dir);
    int path_size = dir_len + 1 + strlen(fname) + 1;

    char_buffer path(path_size);
    if(!path) return NULL;
    strcpy(path, dir);
    path[dir_len] = '\\';
    strcpy(path + dir_len + 1, fname);

    char_buffer name = read_item_name(path);
    int name_size = strlen(name) + 1;

    char_buffer buf(path_size + name_size);
    if(buf)
    {
        strcpy(buf, path);
        strcpy(buf + strlen(buf) + 1, name);
    }

    return buf;
}

void add_item(const char* dir, const ffblk& ffb)
{
    char_buffer buf = read_item_data(dir, ffb.ff_name);
    if(!buf)
    {
        ++skipped_files;
        return;
    }

    if(list_size == list_capacity)
    {
        int extra = !list_capacity ? 4 : min(list_capacity, 1024);
        int cap = list_capacity + extra;
        char* new_list = (char*)realloc(list, sizeof(item) * cap);
        if(!new_list)
        {
            ++skipped_files;
            return;
        }
        memset(new_list + list_capacity * sizeof(item), 0, sizeof(item) * (cap - list_capacity));
        list = (item*)new_list;
        list_capacity = cap;
    }

    const item it = { buf };
    const char* const name = it.name();

    int i = 0;
    for(; i < list_size; ++i)
    {
        if(stricmp(name, list[i].name()) < 0)
        {
            memmove(list + i + 1, list + i, sizeof(item) * (list_size - i));
            memset(&list[i], sizeof(list[i]), 0);
            break;
        }
    }
    list[i].buf = buf.release();
    ++list_size;
    invalid.list = true;
}

bool pattern_match(const char* pattern, const char* str)
{
    for(bool star = false;;)
    {
        if(*pattern == '*')
        {
            star = true;
            ++pattern;
            continue;
        }

        bool eop = !star && !*pattern;

        if(!*str || eop)
            return !*str && !*pattern;

        if(*pattern == '?' || tolower(*str) == tolower(*pattern))
        {
            star = false;
            ++pattern;
        }
        else if(!star)
        {
            return false;
        }

        ++str;
    }
}

void run_loop()
{
    int ff_t0 = ani_time();
    int ff_ani = 0;
    render_progress(0);

    scanner scan;
    bool scan_done = false;
    skipped_dirs = 0;
    skipped_files = 0;

    for(;;)
    {
        if(scan_done || kbhit())
        {
            do
            {
                if(!process_input()) return;
            }
            while(kbhit());
        }

        if(invalid.list) render_list();

        if(!scan_done)
        {
            if(const ffblk* ffb = scan.next())
            {
                if((ffb->ff_attrib & FA_DIREC) == 0)
                    for(const char* filter = option_file_filter; *filter; filter += strlen(filter) + 1)
                    {
                        if(pattern_match(filter, ffb->ff_name))
                        {
                            add_item(scan.dir(), *ffb);
                            break;
                        }
                    }
            }
            else scan_done = true;

            if(scan_done)
            {
                render_top_line();
            }
            else
            {
                int n = ani_frames_since(ff_t0);
                if(n > 0)
                {
                    ff_t0 = ani_time(ff_t0, n);

                    ++ff_ani %= (2 * PROGRESS_BAR_SIZE);
                    render_progress(ff_ani);
                }
            }
        }
    }
}

bool resolve_path(const char* path, const char* outpath)
{
    union REGS regs;
    struct SREGS sregs;
    regs.h.ah = 0x60;
    regs.x.si = FP_OFF(path);
    sregs.ds = FP_SEG(path);
    regs.x.di = FP_OFF(outpath);
    sregs.es = FP_SEG(outpath);
    intdosx(&regs, &regs, &sregs);
    return !regs.x.cflag;
}

void clear_list()
{
    for(int i = 0; i < list_size; ++i)
    {
        free(list[i].buf);
    }
    free(list);
    list = NULL;
    list_size = 0;
    list_capacity = 0;
}

void clear_render_buffer()
{
    free(render_buffer);
    render_buffer = NULL;
    render_buffer_size = 0;
}

bool run()
{
    gettextinfo(&ti);
    list_height = ti.screenheight - 3;

    clrscr();
    render_top_line();
    render_bottom_line();

    list_offset = 0;
    selected_item = -1;

    gotoxy(1, ti.screenheight);
    line_length = 0;
    edit_offset = 0;

    confirmed_action = false;

    run_loop();

    clrscr();

    char* action_path = NULL;
    if(confirmed_action)
    {
        const char* path = list[selected_item].path();
        if((action_path = (char*)alloca(strlen(path) + 1)) != NULL)
            strcpy(action_path, path);
    }

    clear_list();
    clear_render_buffer();

    if(!action_path) return false;
    if(option_noexec)
    {
        fputs(action_path, stdout);
        return false;
    }
    char* backslash = strrchr(action_path, '\\');
    *backslash = 0;
    setdisk(tolower(action_path[0]) - 'a');
    chdir(action_path);
    *backslash = '\\';
    system(action_path);
    return true;
}

int main(int argc, const char* argv[])
{
    char scan_root[MAXPATH] = "";
    {
        char_buffer file_filter;
        size_t file_filter_len = 0;

        for(int i = 1; i < argc; ++i)
        {
            if(argv[i][0] == '/')
            {
                if(!stricmp(argv[i], "/?"))
                {
                    puts(
                        "SLR is an interactive program launcher\n\n"
                        "SLR.EXE [/S] [root] [filename...]\n\n"
                        "  /S       Select only. With this option the selected program will not be\n"
                        "           launched. Instead, its path will be written to standard output.\n"
                        "  root     Directory to be scanned.\n"
                        "  filename File(s) to be included in the program list.\n\n"
                        "Example:\n\n"
                        "  SLR.EXE C:\\GAMES *.BAT *.COM *.EXE\n");

                    return 0;
                }
                else if(!stricmp(argv[i], "/s"))
                {
                    option_noexec = true;
                }
                else
                {
                    fputs("Invalid option: ", stdout);
                    puts(argv[i]);
                    return -1;
                }
            }
            else if(!scan_root[0])
            {
                if(!resolve_path(argv[i], scan_root))
                {
                    fputs("Invalid path: ", stdout);
                    puts(argv[i]);
                    return -1;
                }
            }
            else
            {
                size_t len = strlen(argv[i]) + 1;
                if(file_filter.resize(file_filter_len + len))
                {
                    strcpy(file_filter + file_filter_len, argv[i]);
                    file_filter_len += len;
                }
            }
        }

        if(!scan_root[0]) resolve_path(".", scan_root);
        for(int i = strlen(scan_root); i > 0 && scan_root[i - 1] == '\\'; scan_root[--i] = 0);
        option_scan_root = scan_root;

        if(file_filter_len)
        {
            char* buf = (char*)alloca(file_filter_len + 1);
            if(!buf) return -1;
            memcpy(buf, file_filter, file_filter_len);
            buf[file_filter_len] = 0;
            option_file_filter = buf;
        }
    }

    while(run());
    return 0;
}
