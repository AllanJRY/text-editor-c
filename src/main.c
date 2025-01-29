/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define EDITOR_VERSION    "0.0.1"
#define EDITOR_TAB_STOP   8
#define EDITOR_QUIT_TIMES 1

#define CTRL_KEY(k) ((k) & 0x1f)

typedef enum Editor_Key {
    MOVE_LEFT  = 'h',
    MOVE_RIGHT = 'l',
    MOVE_UP    = 'k',
    MOVE_DOWN  = 'j',
    BACKSPACE  = 127,
    PAGE_UP    = 1000,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
} Editor_Key;

/*** datas ***/

typedef struct Editor_Row Editor_Row;
struct Editor_Row {
    int   size;
    int   render_size;
    char* chars;
    char* render;
};

typedef struct Editor_State Editor_State;
struct Editor_State {
    int            cursor_x, cursor_y;
    int            render_x;
    int            row_offset, col_offset;
    int            screen_rows;
    int            screen_cols;
    int            rows_count;
    Editor_Row*    rows;
    int            dirty;
    char*          filename;
    char           status_msg[80];
    time_t         status_msg_time;
    struct termios original_termios;
};

Editor_State editor_state;

/*** prototypes ***/

void editor_set_status_msg(const char* fmt, ...);
void editorRefreshScreen(void);
char* editor_prompt(char* prompt);

/*** terminal ***/

void die(const char* s) {
    // clear the entire screen.
    write(STDOUT_FILENO, "\x1b[2J", 4);
    // move the cursor to the top-left corner.
    write(STDOUT_FILENO, "\x1b[H", 4);

    perror(s);
    exit(1);
}

void disable_raw_mode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &editor_state.original_termios) == -1) {
        die("`tcsetattr` fail when disabling raw mode");
    }
}

void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &editor_state.original_termios) == -1) {
        die("`tcgetattr` fail when enabling raw mode");
    }
    atexit(disable_raw_mode);

    struct termios raw = editor_state.original_termios;

    // ICRNL Fix Ctrl-M, prevent carriage return `\r` (13) to be transtaled to `\n` (10).
    // IXON disable Ctrl+S and Ctrl+Q.
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // Torn off output processing. This prevent `\n` to be translated to `\r\n`.
    raw.c_oflag &= ~(OPOST);

    raw.c_cflag |= (CS8);

    // ICANON ENABLE read byte by byte, no need to wait for EOF char.
    // IEXTEN disable Ctrl+V.
    // ISIG disable Ctrl+C and Ctrl+Z.
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);


    // Sets the minimum number of bytes of input needed before read() can return.
    raw.c_cc[VMIN] = 0;
    // Sets the maximum amount of time to wait before read() returns (is is in tenths of a second).
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("`tcsetattr` fail when enabling raw mode");
    }
}

int editor_read_key(void) {
    int read_count;
    char c;

    while ((read_count = read(STDIN_FILENO, &c, 1)) != 1) {
        if (read_count == -1 && errno != EAGAIN) {
            die("Error while reading input");
        }
    }

    // Mapp arrow keys to hjkl.
    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';


        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if(seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch(seq[1]) {
                    case 'A': return MOVE_DOWN;
                    case 'B': return MOVE_UP;
                    case 'C': return MOVE_RIGHT;
                    case 'D': return MOVE_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if(seq[0] == 'O') {
            switch(seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

bool get_cursor_position(int* rows, int* cols) {
    char buf[32];
    unsigned int i = 0;

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return false;
    }

    while(i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }

        if (buf[i] == 'R') {
            break;
        }

        i += 1;
    }

    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1] != '[') {
        die("wesh");
        return false;
    }

    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return false;
    }

    return true;
}

bool get_window_size(int* rows, int* cols) {
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return false;
        }

        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return true;
    }
}

/*** row operation ***/

int editor_row_cursor_x_to_render_x(Editor_Row* row, int cursor_x) {
    int render_x = 0;
    for (int j = 0; j < cursor_x; j += 1) {
        if (row->chars[j] == '\t') {
            render_x = (EDITOR_TAB_STOP - 1) - (render_x % EDITOR_TAB_STOP);
        }
        render_x += 1;
    }

    return render_x;
}

void editor_update_row(Editor_Row* row) {
    int tabs = 0;
    for(int j = 0; j < row->size; j += 1) {
        if (row->chars[j] == '\t') tabs =+ 1;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (EDITOR_TAB_STOP - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j += 1) {
        if (row->chars[j] == '\t') {
            row->render[idx] = ' ';
            while (idx % EDITOR_TAB_STOP != 0) {
                row->render[idx] = ' ';
                idx += 1;
            }
        } else {
            row->render[idx] = row->chars[j];
            idx += 1;
        }
    }

    row->render[idx] = '\0';
    row->render_size = idx;
}


void editor_insert_row(int at, char* line, size_t line_len) {
    if (at < 0 || at > editor_state.rows_count) return;

    editor_state.rows = realloc(editor_state.rows, sizeof(Editor_Row) * (editor_state.rows_count + 1));
    memmove(&editor_state.rows[at + 1], &editor_state.rows[at], sizeof(Editor_Row) * (editor_state.rows_count - at));

    editor_state.rows[at].size = line_len;
    editor_state.rows[at].chars = malloc(line_len + 1);
    memcpy(editor_state.rows[at].chars, line, line_len);
    editor_state.rows[at].chars[line_len] = '\0';

    editor_state.rows[at].render_size = 0;
    editor_state.rows[at].render = NULL;
    editor_update_row(&editor_state.rows[at]);

    editor_state.rows_count += 1;
    editor_state.dirty      += 1;
}

void editor_free_row(Editor_Row* row) {
    free(row->render);
    free(row->chars);
}

void editor_del_row(int at) {
    if (at < 0 || at >= editor_state.rows_count) return;
    editor_free_row(&editor_state.rows[at]);
    memmove(&editor_state.rows[at], &editor_state.rows[at + 1], sizeof(Editor_Row) * (editor_state.rows_count - at - 1));
    editor_state.rows_count -= 1;
    editor_state.dirty += 1;
}

void editor_row_insert_char(Editor_Row* row, int at, int c) {
    if(at < 0 || at > row->size) {
        at = row->size;
    }

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size += 1;
    row->chars[at] = c;
    editor_update_row(row);
    editor_state.dirty += 1;
}

void editor_row_append_string(Editor_Row* row, char* s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    editor_state.dirty += 1;
}

void editor_row_del_char(Editor_Row* row, int at) {
    if(at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size -= 1;
    editor_update_row(row);
    editor_state.dirty += 1;
}

/*** editor operations ***/

void editor_insert_char(int c) {
    if(editor_state.cursor_y == editor_state.rows_count) {
        editor_insert_row(editor_state.rows_count, "", 0);
    }

    editor_row_insert_char(&editor_state.rows[editor_state.cursor_y], editor_state.cursor_x, c);
    editor_state.cursor_x += 1;
}

void editor_insert_new_line(void) {
    if (editor_state.cursor_x == 0) {
        editor_insert_row(editor_state.cursor_y, "", 0);
    } else {
        Editor_Row* row = &editor_state.rows[editor_state.cursor_y];
        editor_insert_row(editor_state.cursor_y + 1, &row->chars[editor_state.cursor_x], row->size - editor_state.cursor_x);
        row = &editor_state.rows[editor_state.cursor_y];
        row->size = editor_state.cursor_x;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }

    editor_state.cursor_y += 1;
    editor_state.cursor_x  = 0;
}

void editor_del_char(void) {
    if (editor_state.cursor_y == editor_state.rows_count) return;
    if (editor_state.cursor_x == 0 && editor_state.cursor_y == 0) return;


    Editor_Row* row = &editor_state.rows[editor_state.cursor_y];
    if (editor_state.cursor_x > 0) {
        editor_row_del_char(row, editor_state.cursor_x - 1);
        editor_state.cursor_x -= 1;
    } else {
        editor_state.cursor_x = editor_state.rows[editor_state.cursor_y - 1].size;
        editor_row_append_string(&editor_state.rows[editor_state.cursor_y - 1], row->chars, row->size);
        editor_del_row(editor_state.cursor_y);
        editor_state.cursor_y -= 1;
    }
}

/*** file i/o ***/

char* editor_rows_to_string(int* buf_len) {
    int total_len = 0;
    for (int j = 0; j < editor_state.rows_count; j += 1) {
        total_len += editor_state.rows[j].size + 1;
    }
    *buf_len = total_len;

    char* buf = malloc(total_len);
    char *p = buf;
    for (int j = 0; j < editor_state.rows_count; j += 1) {
        memcpy(p, editor_state.rows[j].chars, editor_state.rows[j].size);
        p += editor_state.rows[j].size;
        *p = '\n';
        p += 1;
    }

    return buf;
}

void editor_open(char* filename) {
    free(editor_state.filename);
    editor_state.filename = strdup(filename);

    FILE* fp = fopen(filename, "r");
    if (!fp) die("Error while opening the file");

    char* line       = NULL;
    size_t line_cap  = 0;
    ssize_t line_len;

    while((line_len = getline(&line, &line_cap, fp)) != -1) {
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line_len -= 1;
        }

        editor_insert_row(editor_state.rows_count, line, line_len);
    }

    free(line);
    fclose(fp);
    editor_state.dirty = 0;
}

void editor_save(void) {
    if (editor_state.filename == NULL) {
        editor_state.filename = editor_prompt("Save as: %s (ESC to cancel)");
        if (editor_state.filename == NULL) {
            editor_set_status_msg("Save aborted");
            return;
        }
    }

    int len;
    char* buf = editor_rows_to_string(&len);

    int fd = open(editor_state.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if(ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                editor_state.dirty = 0;
                editor_set_status_msg("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editor_set_status_msg("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/

typedef struct Append_Buf {
    char* b;
    int len;
} Append_Buf;

#define APPEND_BUF_INIT { .b = NULL, .len = 0 }

void append_buf_append(Append_Buf* buf, const char* s, int len) {
    char *new = realloc(buf->b, buf->len + len);

    if (new == NULL) return;

    memcpy(&new[buf->len], s, len);
    buf->b    = new;
    buf->len += len;
}

void append_buf_free(Append_Buf* buf) {
    free(buf->b);
}

/*** output ***/

void editor_scroll(void) {
    editor_state.render_x = 0;
    if(editor_state.cursor_y < editor_state.rows_count) {
        editor_state.render_x = editor_row_cursor_x_to_render_x(&editor_state.rows[editor_state.cursor_y], editor_state.cursor_x);
    }

    if (editor_state.cursor_y < editor_state.row_offset) {
        editor_state.row_offset = editor_state.cursor_y;
    }
    if (editor_state.cursor_y >= editor_state.row_offset + editor_state.screen_rows) {
        editor_state.row_offset = editor_state.cursor_y - editor_state.screen_rows + 1;
    }
    if (editor_state.render_x < editor_state.col_offset) {
        editor_state.col_offset = editor_state.render_x;
    }
    if (editor_state.render_x >= editor_state.col_offset + editor_state.screen_cols) {
        editor_state.col_offset = editor_state.render_x - editor_state.screen_cols + 1;
    }
}

void editor_draw_rows(Append_Buf* buf) {
    for(int y = 0; y < editor_state.screen_rows; y++) {
        int file_row = y + editor_state.row_offset;
        if (file_row >= editor_state.rows_count) {
            if(editor_state.rows_count == 0 && y == editor_state.screen_rows / 3) {
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome), "Editeur -- version %s", EDITOR_VERSION);

                if (welcome_len > editor_state.screen_cols) {
                    welcome_len = editor_state.screen_cols;
                }

                int padding = (editor_state.screen_cols - welcome_len) / 2;
                if (padding) {
                    append_buf_append(buf, "~", 1);
                }

                while (padding -= 1) {
                    append_buf_append(buf, " ", 1);
                }

                append_buf_append(buf, welcome, welcome_len);
            }
            else {
                append_buf_append(buf, "~", 1);
            }
        } else {
            int len = editor_state.rows[file_row].render_size - editor_state.col_offset;
            if(len < 0) len = 0;
            if (len > editor_state.screen_cols) len = editor_state.screen_cols;
            append_buf_append(buf, &editor_state.rows[file_row].render[editor_state.col_offset], len);
        }

        // clear the current line.
        append_buf_append(buf, "\x1b[K", 3);
        append_buf_append(buf, "\r\n", 2);
    }
}

void editor_draw_status_bar(Append_Buf* buf) {
    append_buf_append(buf, "\x1b[7m", 4);

    char status[80], right_status[80];

    int len = snprintf(
        status,
        sizeof(status),
        "%.20s - %d lines %s",
        editor_state.filename ? editor_state.filename : "[No Name]",
        editor_state.rows_count,
        editor_state.dirty ? "(modified)" : ""
    );
    if(len > editor_state.screen_cols) {
        len = editor_state.screen_cols;
    }
    append_buf_append(buf, status, len);

    int right_len = snprintf(right_status, sizeof(right_status), "%d/%d", editor_state.cursor_y + 1, editor_state.rows_count);

    while(len < editor_state.screen_cols) {
        if(editor_state.screen_cols - len == right_len) {
            append_buf_append(buf, right_status, right_len);
            break;
        } else {
            append_buf_append(buf, " ", 1);
            len += 1;
        }
    }
    append_buf_append(buf, "\x1b[m", 3);
    append_buf_append(buf, "\r\n", 2);
}

void editor_draw_msg_bar(Append_Buf* buf) {
    append_buf_append(buf, "\x1b[K", 3);
    int msg_len = strlen(editor_state.status_msg);
    
    if(msg_len > editor_state.screen_cols) {
        msg_len = editor_state.screen_cols;
    }

    if(msg_len && time(NULL) - editor_state.status_msg_time < 5) {
        append_buf_append(buf, editor_state.status_msg, msg_len);
    }
}

void editor_refresh_screen(void) {
    editor_scroll();

    Append_Buf buf = APPEND_BUF_INIT;
    
    // hide the cursor
    append_buf_append(&buf, "\x1b[?25l", 6);

    // clear the entire screen, not used because each line is cleared in editor_draw_rows.
    // append_buf_append(&buf, "\x1b[2J", 4);

    // move the cursor to the top-left corner.
    append_buf_append(&buf, "\x1b[H", 3);

    editor_draw_rows(&buf);
    editor_draw_status_bar(&buf);
    editor_draw_msg_bar(&buf);

    char cursor_buf[32];
    snprintf(
        cursor_buf,
        sizeof(cursor_buf),
        "\x1b[%d;%dH",
        (editor_state.cursor_y - editor_state.row_offset) + 1,
        (editor_state.render_x - editor_state.col_offset) + 1
    );
    append_buf_append(&buf, cursor_buf, strlen(cursor_buf));

    // show the cursor
    append_buf_append(&buf, "\x1b[?25h", 6);

    write(STDOUT_FILENO, buf.b, buf.len);
    append_buf_free(&buf);
}

void editor_set_status_msg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(editor_state.status_msg, sizeof(editor_state.status_msg), fmt, ap);
    va_end(ap);
    editor_state.status_msg_time = time(NULL);
}

/*** input ***/

char* editor_prompt(char* prompt) {
    size_t buf_cap = 128;
    char* buf = malloc(buf_cap);

    size_t buf_len = 0;
    buf[0] = '\0';

    while(1) {
        editor_set_status_msg(prompt, buf);
        editor_refresh_screen();
        int c = editor_read_key();
        if (c == '\x1b') {
            editor_set_status_msg("");
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buf_len != 0) {
                editor_set_status_msg("");
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buf_len == buf_cap - 1) {
                buf_cap *= 2;
                buf = realloc(buf, buf_cap);
            }

            buf[buf_len]  = c;
            buf_len      += 1;
            buf[buf_len]  = '\0';
        }
    }
}

void editor_move_cursor(int key_pressed) {
    Editor_Row* row = (editor_state.cursor_y >= editor_state.rows_count) ? NULL : &editor_state.rows[editor_state.cursor_y];

    switch (key_pressed) {
        case MOVE_LEFT:
            if(editor_state.cursor_x != 0) {
                editor_state.cursor_x -= 1;
            } else if (editor_state.cursor_y > 0) {
                editor_state.cursor_y -= 1;
                editor_state.cursor_x = editor_state.rows[editor_state.cursor_y].size;
            }
            break;
        case MOVE_RIGHT:
            if (row && editor_state.cursor_x < row->size) {
                editor_state.cursor_x += 1;
            } else if (row && editor_state.cursor_x == row->size) {
                editor_state.cursor_y += 1;
                editor_state.cursor_x = 0;
            }
            break;
        case MOVE_UP:
            if(editor_state.cursor_y != 0) {
                editor_state.cursor_y -= 1;
            }
            break;
        case MOVE_DOWN:
            if(editor_state.cursor_y < editor_state.rows_count) {
                editor_state.cursor_y += 1;
            }
            break;
    }

    row = (editor_state.cursor_y >= editor_state.rows_count) ? NULL : &editor_state.rows[editor_state.cursor_y];
    int row_len = row ? row->size : 0;
    if(editor_state.cursor_x > row_len) {
        editor_state.cursor_x = row_len;
    }
}


void editor_process_keypress(void) {
    static int quit_times = EDITOR_QUIT_TIMES;
    int c = editor_read_key();

    switch(c) {
        case CTRL_KEY('\r'):
            editor_insert_new_line();
            break;

        case CTRL_KEY('q'):
            if (editor_state.dirty && quit_times > 0) {
                editor_set_status_msg("WARNING! File has unsaved changes. Press Ctrl-Q %d more times de quite.", quit_times);
                quit_times -= 1;
                return;
            }
            editor_refresh_screen();
            exit(0);
            break;

        case CTRL_KEY('s'):
            editor_save();
            break;

        case HOME_KEY:
            editor_state.cursor_x = 0;
            break;

        case END_KEY:
            if(editor_state.cursor_y < editor_state.rows_count) {
                editor_state.cursor_x = editor_state.rows[editor_state.cursor_y].size;
            }
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY) editor_move_cursor(MOVE_RIGHT);
            editor_del_char();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    editor_state.cursor_y = editor_state.row_offset;
                } else if (c == PAGE_DOWN) {
                    editor_state.cursor_y = editor_state.row_offset + editor_state.screen_rows - 1;
                    if(editor_state.cursor_y > editor_state.rows_count) {
                        editor_state.cursor_y = editor_state.rows_count;
                    }
                }

                int times = editor_state.screen_rows;
                while (times -= 1) {
                    editor_move_cursor(c == PAGE_UP ? MOVE_UP : MOVE_DOWN);
                }
            }
            break;
        case MOVE_LEFT:
        case MOVE_DOWN:
        case MOVE_UP:
        case MOVE_RIGHT:
            editor_move_cursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editor_insert_char(c);
            break;
    }

    quit_times = EDITOR_QUIT_TIMES;
}

/*** init ***/

void editor_init(void) {
    editor_state.cursor_x        = 0;
    editor_state.cursor_y        = 0;
    editor_state.render_x        = 0;
    editor_state.row_offset      = 0;
    editor_state.col_offset      = 0;
    editor_state.rows            = NULL;
    editor_state.rows_count      = 0;
    editor_state.dirty           = 0;
    editor_state.filename        = NULL;
    editor_state.status_msg[0]   = '\0';
    editor_state.status_msg_time = 0;

    if(!get_window_size(&editor_state.screen_rows, &editor_state.screen_cols)) {
        die("Error during editor init");
    }

    editor_state.screen_rows -= 2;
}

int main(int argc, char* argv[]) {
    enable_raw_mode();
    editor_init();

    if (argc >= 2) {
        editor_open(argv[1]);
    }

    editor_set_status_msg("HELP: Ctrl-S = save | Ctrl-Q = quit");

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}

