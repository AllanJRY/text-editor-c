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

/*
 * Multiline 
 * comment
 * test
 */

/*** defines ***/

#define EDITOR_VERSION    "0.0.1"
#define EDITOR_TAB_STOP   8
#define EDITOR_QUIT_TIMES 1

#define CTRL_KEY(k) ((k) & 0x1f)

typedef enum Editor_Key {
    BACKSPACE  = 127,
    MOVE_LEFT  = 1000,
    MOVE_RIGHT,
    MOVE_UP,
    MOVE_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
} Editor_Key;

typedef enum Editor_Highlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
} Editor_Highlight;

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/

typedef struct Editor_Syntax {
    char*  file_type;
    char** file_match;
    char** keywords;
    char*  singleline_comment_start;
    char*  multiline_comment_start;
    char*  multiline_comment_end;
    int    flags;
} Editor_Syntax;

typedef struct Editor_Row {
    int            idx;
    int            size;
    int            render_size;
    char*          chars;
    char*          render;
    unsigned char* hl;
    int            hl_open_comment;
} Editor_Row;

typedef struct Editor_State {
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
    Editor_Syntax* syntax;
    struct termios original_termios;
} Editor_State;

Editor_State editor_state;

/*** filetypes ***/

char* c_hl_extensions[] = { ".c", ".h", ".cpp", NULL };
char* c_hl_keywords[]   = { 
  "switch", "if", "while", "for", "break", "continue", "return", "else",
  "struct", "union", "typedef", "static", "enum", "class", "case",

  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", NULL
};

Editor_Syntax HLDB[] = {
    {"c", c_hl_extensions, c_hl_keywords, "//", "/*", "*/", HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS },
};

#define HLDB_COUNT (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

void editor_set_status_msg(const char* fmt, ...);
void editorRefreshScreen(void);
char* editor_prompt(char* prompt, void (*callback)(char*, int));

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

/*** syntax highlighting ***/

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editor_update_syntax(Editor_Row* row) {
    row->hl = realloc(row->hl, row->render_size);
    memset(row->hl, HL_NORMAL, row->render_size);

    if (editor_state.syntax == NULL) return;

    char** keywords = editor_state.syntax->keywords;

    char* scs = editor_state.syntax->singleline_comment_start;
    char* mcs = editor_state.syntax->multiline_comment_start;
    char* mce = editor_state.syntax->multiline_comment_end;
    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep   = 1;
    int in_string  = 0;
    int in_comment = (row->idx > 0 && editor_state.rows[row->idx - 1].hl_open_comment);

    int i = 0;
    while(i < row->render_size) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if(scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->render_size - i);
                break;
            }
        }

        if(mcs_len && mce_len && !in_string) {
            if(in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i          += mce_len;
                    in_comment  = 0;
                    prev_sep    = 1;
                    continue;
                } else {
                    i += 1;
                    continue;
                }
            } else if(!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i          += mcs_len;
                in_comment  = 1;
                continue;
            }
        }

        if (editor_state.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->render_size) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if(c == in_string) in_string = 0;
                i += 1;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i += 1;
                    continue;
                }
            }
        }

        if (editor_state.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i += 1;
                prev_sep = 0;
                continue;
            }
        }

        if(prev_sep) {
            int j;
            for(j = 0; keywords[j]; j += 1) {
                int k_len = strlen(keywords[j]);
                int kw2 = keywords[j][k_len - 1] == '|';
                if (kw2) k_len -= 1;

                if(!strncmp(&row->render[i], keywords[j], k_len) && is_separator(row->render[i + k_len])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, k_len);
                    i += k_len;
                    break;
                }
            }

            if(keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i += 1;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < editor_state.rows_count) {
        editor_update_syntax(&editor_state.rows[row->idx + 1]);
    }
}

int editor_syntax_to_color(int hl) {
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36;
        case HL_KEYWORD1:  return 33;
        case HL_KEYWORD2:  return 32;
        case HL_STRING:    return 35;
        case HL_NUMBER:    return 31;
        case HL_MATCH:     return 34;
        default:           return 37;
    }
}

void editor_select_syntax_highlight(void) {
    editor_state.syntax = NULL;
    if (editor_state.filename == NULL) return;

    char* ext = strrchr(editor_state.filename, '.');

    for(unsigned int j = 0; j < HLDB_COUNT; j += 1) {
        Editor_Syntax* stx = &HLDB[j];

        unsigned int i = 0;
        while (stx->file_match[i]) {
            int is_ext = (stx->file_match[i][0] == '.');
            if ( (is_ext && ext && !strcmp(ext, stx->file_match[i])) || 
                 (!is_ext && strstr(editor_state.filename, stx->file_match[i])) ) {
                editor_state.syntax = stx;

                for(int file_row = 0; file_row < editor_state.rows_count; file_row += 1) {
                    editor_update_syntax(&editor_state.rows[file_row]);
                }

                return;
            }

            i += 1;
        }
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

int editor_row_render_x_to_cursor_x(Editor_Row* row, int render_x) {
    int curr_render_x = 0;
    int cursor_x;
    for (cursor_x = 0; cursor_x < row->size; cursor_x += 1) {
        if (row->chars[cursor_x] == '\t') {
            curr_render_x += (EDITOR_TAB_STOP - 1) - (curr_render_x % EDITOR_TAB_STOP);
        }
        curr_render_x += 1;

        if (curr_render_x > render_x) return cursor_x;
    }

    return cursor_x;
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

    editor_update_syntax(row);
}


void editor_insert_row(int at, char* line, size_t line_len) {
    if (at < 0 || at > editor_state.rows_count) return;

    editor_state.rows = realloc(editor_state.rows, sizeof(Editor_Row) * (editor_state.rows_count + 1));
    memmove(&editor_state.rows[at + 1], &editor_state.rows[at], sizeof(Editor_Row) * (editor_state.rows_count - at));
    for(int j = at + 1; j <= editor_state.rows_count; j += 1) editor_state.rows[j].idx += 1;

    editor_state.rows[at].idx = at;

    editor_state.rows[at].size  = line_len;
    editor_state.rows[at].chars = malloc(line_len + 1);
    memcpy(editor_state.rows[at].chars, line, line_len);
    editor_state.rows[at].chars[line_len] = '\0';

    editor_state.rows[at].render_size     = 0;
    editor_state.rows[at].render          = NULL;
    editor_state.rows[at].hl              = NULL;
    editor_state.rows[at].hl_open_comment = 0;
    editor_update_row(&editor_state.rows[at]);

    editor_state.rows_count += 1;
    editor_state.dirty      += 1;
}

void editor_free_row(Editor_Row* row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editor_del_row(int at) {
    if (at < 0 || at >= editor_state.rows_count) return;
    editor_free_row(&editor_state.rows[at]);
    memmove(&editor_state.rows[at], &editor_state.rows[at + 1], sizeof(Editor_Row) * (editor_state.rows_count - at - 1));
    for(int j = at; j < editor_state.rows_count - 1; j += 1) editor_state.rows[j].idx -= 1;
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

    editor_select_syntax_highlight();

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
        editor_state.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if (editor_state.filename == NULL) {
            editor_set_status_msg("Save aborted");
            return;
        }

        editor_select_syntax_highlight();
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

/*** find ***/

void editor_find_callback(char* query, int key) {
    static int last_match_row = -1;
    static int direction      = 1;

    static int saved_hl_line;
    static char* saved_hl = NULL;

    if (saved_hl) {
        memcpy(editor_state.rows[saved_hl_line].hl, saved_hl, editor_state.rows[saved_hl_line].render_size);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        last_match_row = -1;
        direction  = 1;
        return;
    } else if(key == MOVE_RIGHT || key == MOVE_DOWN) {
        direction  = 1;
    } else if(key == MOVE_LEFT || key == MOVE_UP) {
        direction  = -1;
    } else {
        last_match_row = -1;
        direction  = 1;
    }

    if (last_match_row == -1) direction = 1;
    int curr_match_row = last_match_row;
    for (int i = 0; i < editor_state.rows_count;  i += 1) {
        curr_match_row += direction;
        if (curr_match_row  == -1) curr_match_row = editor_state.rows_count - 1;
        else if (curr_match_row == editor_state.rows_count) curr_match_row = 0; 

        Editor_Row* row = &editor_state.rows[curr_match_row];
        char* match = strstr(row->render, query);
        if (match) {
            last_match_row = curr_match_row;
            editor_state.cursor_y = curr_match_row;
            editor_state.cursor_x = editor_row_render_x_to_cursor_x(row, match - row->render);
            editor_state.row_offset = editor_state.rows_count;

            saved_hl_line = curr_match_row;
            saved_hl = malloc(row->render_size);
            memcpy(saved_hl, row->hl, row->render_size);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editor_find(void) {
    int saved_cursor_x   = editor_state.cursor_x;
    int saved_cursor_y   = editor_state.cursor_y;
    int saved_col_offset = editor_state.col_offset;
    int saved_row_offset = editor_state.row_offset;

    char* query = editor_prompt("Search: %s (Use ESC/Arrows/Enter)", editor_find_callback);
    if (query) {
        free(query);
    } else {
        editor_state.cursor_x   = saved_cursor_x;
        editor_state.cursor_y   = saved_cursor_y;
        editor_state.col_offset = saved_col_offset;
        editor_state.row_offset = saved_row_offset;
    }
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
            char* c  = &editor_state.rows[file_row].render[editor_state.col_offset];
            unsigned char* hl = &editor_state.rows[file_row].hl[editor_state.col_offset];
            int current_color = -1;

            for(int j = 0; j < len; j += 1) {
                if (iscntrl(c[j])) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    append_buf_append(buf, "\x1b[7m", 4);
                    append_buf_append(buf, &sym, 1);
                    append_buf_append(buf, "\x1b[m", 3);
                    if(current_color != -1) {
                        char color_buf[16];
                        int color_len = snprintf(color_buf, sizeof(color_buf), "\x1b[%dm", current_color);
                        append_buf_append(buf, color_buf, color_len);
                    }
                } else if(hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        append_buf_append(buf, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    append_buf_append(buf, &c[j], 1);
                } else {
                    int color = editor_syntax_to_color(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char color_buf[16];
                        int color_len = snprintf(color_buf, sizeof(color_buf), "\x1b[%dm", color);
                        append_buf_append(buf, color_buf, color_len);
                    }
                    append_buf_append(buf, &c[j], 1);
                }
            }

            append_buf_append(buf, "\x1b[39m", 5);
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

    int right_len = snprintf(
        right_status,
        sizeof(right_status),
        "%s | %d/%d",
        editor_state.syntax ? editor_state.syntax->file_type : "no ft",
        editor_state.cursor_y + 1,
        editor_state.rows_count
    );

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

char* editor_prompt(char* prompt, void (*callback)(char*, int)) {
    size_t buf_cap = 128;
    char* buf = malloc(buf_cap);

    size_t buf_len = 0;
    buf[0] = '\0';

    while(1) {
        editor_set_status_msg(prompt, buf);
        editor_refresh_screen();
        int c = editor_read_key();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buf_len != 0) {
                buf_len      -= 1;
                buf[buf_len]  = '\0';
            }
        } else if (c == '\x1b') {
            editor_set_status_msg("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buf_len != 0) {
                editor_set_status_msg("");
                if (callback) callback(buf, c);
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

        if (callback) callback(buf, c);
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
        case MOVE_DOWN:
            if(editor_state.cursor_y != 0) {
                editor_state.cursor_y -= 1;
            }
            break;
        case MOVE_UP:
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

        case CTRL_KEY('f'):
            editor_find();
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
    editor_state.syntax          = NULL;

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

    editor_set_status_msg("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}

