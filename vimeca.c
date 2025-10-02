#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <ctype.h>
#include <sys/time.h>

#define CTRL_KEY(k) ((k) & 0x1f)

enum EditorMode {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_COMMAND
};

struct EditorState {
    int fd;
    char *mapped;
    size_t file_size;
    size_t offset;
    size_t cursor_x;
    size_t cursor_y;
    size_t col_offset;
    size_t row_offset;
    size_t screen_row_offset;
    int screen_rows;
    int screen_cols;
    enum EditorMode mode;
    char command_buf[256];
    int command_len;
    char status_msg[256];
    struct termios orig_termios;
    char *content_snapshot;
};

struct EditorState E;

void die(const char *s) {
    ssize_t __attribute__((unused)) ret;
//    ret = write(STDOUT_FILENO, "\x1b[2J", 4);
//    ret = write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int getWindowSize(int *rows, int *cols) {
    int ret;
    struct winsize ws;
    ws.ws_col = 0;
    ws.ws_row = 0;

    ret = ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);

    if ( ret != -1 ) {
	if ( ws.ws_col != 0) {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
	} else {
	    *cols = 80;
	    *rows = 25;
	}
    return 0;
    }

    return -1;
}

size_t getLineStart(size_t pos) {
    while (pos > E.offset && E.mapped[pos - 1] != '\n') {
        pos--;
    }
    return pos;
}

size_t getLineEnd(size_t pos) {
    while (pos < E.offset + E.file_size && E.mapped[pos] != '\n') {
        pos++;
    }
    return pos;
}

size_t getCursorPos() {
    size_t pos = E.offset;
    size_t y = 0;

    while (pos < E.offset + E.file_size && y < E.cursor_y) {
        if (E.mapped[pos] == '\n') y++;
        pos++;
    }

    size_t x = 0;
    while (pos < E.offset + E.file_size && x < E.cursor_x && E.mapped[pos] != '\n') {
        pos++;
        x++;
    }

    return pos;
}

size_t countLines() {
    size_t lines = 1;
    for (size_t i = E.offset; i < E.offset + E.file_size; i++) {
        if (E.mapped[i] == '\n') lines++;
    }
    return lines;
}

void moveCursor(int key) {
    size_t pos = getCursorPos();
    size_t line_start = getLineStart(pos);
    size_t line_end = getLineEnd(pos);

    switch (key) {
        case 'h':
            if (E.cursor_x > 0) {
                E.cursor_x--;
            } else if (E.cursor_y > 0) {
                E.cursor_y--;
                pos = getCursorPos();
                line_end = getLineEnd(getLineStart(pos));
                E.cursor_x = line_end - getLineStart(pos);
            }
            break;
        case 'l':
            if (pos < E.offset + E.file_size && E.mapped[pos] != '\n') {
                E.cursor_x++;
            } else if (pos < E.offset + E.file_size - 1 && E.mapped[pos] == '\n') {
                E.cursor_y++;
                E.cursor_x = 0;
            }
            break;
        case 'k':
            if (E.cursor_y > 0) {
                E.cursor_y--;
                if (E.cursor_y < E.screen_row_offset) {
                    E.screen_row_offset = E.cursor_y;
                }
            }
            break;
        case 'j':
            if (E.cursor_y < countLines() - 1) {
                E.cursor_y++;
                if (E.cursor_y >= E.screen_row_offset + (E.screen_rows - 1)) {
                    E.screen_row_offset = E.cursor_y - (E.screen_rows - 1) + 1;
                }
            }
            break;
    }

    pos = getCursorPos();
    line_start = getLineStart(pos);
    line_end = getLineEnd(line_start);
    size_t line_len = line_end - line_start;
    if (E.cursor_x > line_len) {
        E.cursor_x = line_len;
    }
}

void insertChar(char c) {
    size_t pos = getCursorPos();
    if (pos < E.offset + E.file_size - 1) {
        memmove(&E.mapped[pos + 1], &E.mapped[pos], E.offset + E.file_size - pos - 1);
        E.mapped[pos] = c;
        if (c == '\n') {
            E.cursor_y++;
            E.cursor_x = 0;
        } else {
            E.cursor_x++;
        }
        msync(E.mapped + E.offset, E.file_size, MS_SYNC);
    }
}

void drawRows(char *buf, int *buf_len) {
    size_t pos = E.offset + E.row_offset;

    for (int y = 0; y < E.screen_rows - 1; y++) {
        size_t line_start = pos;
        size_t line_end = getLineEnd(line_start);

        if (line_start >= E.offset + E.file_size) {
            buf[(*buf_len)++] = '~';
        } else {
            size_t len = line_end - line_start;
            if (len > E.col_offset) {
                size_t display_len = len - E.col_offset;
                if (display_len > (size_t)E.screen_cols) display_len = E.screen_cols;
                memcpy(&buf[*buf_len], &E.mapped[line_start + E.col_offset], display_len);
                *buf_len += display_len;
            }
        }

        buf[(*buf_len)++] = '\x1b';
        buf[(*buf_len)++] = '[';
        buf[(*buf_len)++] = 'K';
        if (y < E.screen_rows - 2) {
            buf[(*buf_len)++] = '\r';
            buf[(*buf_len)++] = '\n';
        }

        if (line_end < E.offset + E.file_size) {
            pos = line_end + 1;
        } else {
            pos = E.offset + E.file_size;
        }
    }
}

void drawStatusBar(char *buf, int *buf_len) {
    buf[(*buf_len)++] = '\r';
    buf[(*buf_len)++] = '\n';
    buf[(*buf_len)++] = '\x1b';
    buf[(*buf_len)++] = '[';
    buf[(*buf_len)++] = '7';
    buf[(*buf_len)++] = 'm';

    char status[256];
    int len;

    if (E.mode == MODE_COMMAND) {
        len = snprintf(status, sizeof(status), ":%s", E.command_buf);
    } else {
        const char *mode_str = E.mode == MODE_INSERT ? "INSERT" : "NORMAL";
        len = snprintf(status, sizeof(status), " %s | Pos: %zu | Offset: 0x%zx ",
                      mode_str, getCursorPos() - E.offset, E.offset);
    }

    if (len > E.screen_cols) len = E.screen_cols;
    memcpy(&buf[*buf_len], status, len);
    *buf_len += len;

    while (len < E.screen_cols) {
        buf[(*buf_len)++] = ' ';
        len++;
    }

    buf[(*buf_len)++] = '\x1b';
    buf[(*buf_len)++] = '[';
    buf[(*buf_len)++] = 'm';
}

void refreshScreen() {
    E.row_offset = 0;
    size_t pos = E.offset;
    for (size_t y = 0; y < E.screen_row_offset && pos < E.offset + E.file_size; y++) {
        while (pos < E.offset + E.file_size && E.mapped[pos] != '\n') pos++;
        if (pos < E.offset + E.file_size) pos++;
        E.row_offset = pos - E.offset;
    }

    if (E.cursor_x < E.col_offset) {
        E.col_offset = E.cursor_x;
    }
    if (E.cursor_x >= E.col_offset + E.screen_cols) {
        E.col_offset = E.cursor_x - E.screen_cols + 1;
    }

    char buf[65536];
    int buf_len = 0;

    buf[buf_len++] = '\x1b';
    buf[buf_len++] = '[';
    buf[buf_len++] = '?';
    buf[buf_len++] = '2';
    buf[buf_len++] = '5';
    buf[buf_len++] = 'l';
    buf[buf_len++] = '\x1b';
    buf[buf_len++] = '[';
    buf[buf_len++] = 'H';

    drawRows(buf, &buf_len);
    drawStatusBar(buf, &buf_len);

    char cursor_buf[32];
    snprintf(cursor_buf, sizeof(cursor_buf), "\x1b[%zu;%zuH",
             E.cursor_y - E.screen_row_offset + 1,
             E.cursor_x - E.col_offset + 1);
    int cursor_len = strlen(cursor_buf);
    memcpy(&buf[buf_len], cursor_buf, cursor_len);
    buf_len += cursor_len;

    buf[buf_len++] = '\x1b';
    buf[buf_len++] = '[';
    buf[buf_len++] = '?';
    buf[buf_len++] = '2';
    buf[buf_len++] = '5';
    buf[buf_len++] = 'h';

    ssize_t __attribute__((unused)) ret;
    ret = write(STDOUT_FILENO, buf, buf_len);
}

void processCommand() {
    ssize_t __attribute__((unused)) ret;
    if (E.command_len == 1 && E.command_buf[0] == 'q') {
        ret = write(STDOUT_FILENO, "\x1b[2J", 4);
        ret = write(STDOUT_FILENO, "\x1b[H", 3);
        munmap(E.mapped, E.file_size);
        close(E.fd);
        exit(0);
    } else if (E.command_len == 1 && E.command_buf[0] == 'w') {
        msync(E.mapped + E.offset, E.file_size, MS_SYNC);
        snprintf(E.status_msg, sizeof(E.status_msg), "File synced");
    }

    E.mode = MODE_NORMAL;
    E.command_len = 0;
    E.command_buf[0] = '\0';
}

int hasContentChanged() {
    return memcmp(E.content_snapshot + E.offset, E.mapped + E.offset, E.file_size - E.offset) != 0;
}

void updateSnapshot() {
    memcpy(E.content_snapshot + E.offset, E.mapped + E.offset, E.file_size - E.offset);
}

int readKeyNonBlocking() {
    char c;
    ssize_t nread = read(STDIN_FILENO, &c, 1);
    if (nread != 1) return -1;

    if (c == '\x1b') {
        char seq[3];

        struct timeval tv;
        fd_set readfds;

        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 1000;

        if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) <= 0) {
            return '\x1b';
        }

        int nread1 = read(STDIN_FILENO, &seq[0], 1);
        if (nread1 != 1) return '\x1b';

        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 1000;

        if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) <= 0) {
            return '\x1b';
        }

        int nread2 = read(STDIN_FILENO, &seq[1], 1);
        if (nread2 != 1) return '\x1b';

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'k';
                case 'B': return 'j';
                case 'C': return 'l';
                case 'D': return 'h';
            }
        }
        return '\x1b';
    }
    return c;
}

int readKey() {
    return readKeyNonBlocking();
}

void processKeypress() {
    int c = readKey();
    if (c == -1) return;

    if (E.mode == MODE_COMMAND) {
        if (c == '\r' || c == '\n') {
            processCommand();
        } else if (c == '\x1b') {
            E.mode = MODE_NORMAL;
            E.command_len = 0;
            E.command_buf[0] = '\0';
        } else if (c == 127 || c == '\b') {
            if (E.command_len > 0) {
                E.command_len--;
                E.command_buf[E.command_len] = '\0';
            } else {
                E.mode = MODE_NORMAL;
            }
        } else if (isprint(c)) {
            if (E.command_len < (int)sizeof(E.command_buf) - 1) {
                E.command_buf[E.command_len++] = c;
                E.command_buf[E.command_len] = '\0';
            }
        }
        return;
    }

    if (E.mode == MODE_INSERT) {
        if (c == '\x1b') {
            E.mode = MODE_NORMAL;
        } else if (c == 127 || c == '\b') {
            if (E.cursor_x > 0) {
                E.cursor_x--;
            }
        } else if (c == '\r') {
            insertChar('\n');
        } else if (isprint(c)) {
            insertChar(c);
        }
        return;
    }

    switch (c) {
        case ':':
            E.mode = MODE_COMMAND;
            E.command_len = 0;
            E.command_buf[0] = '\0';
            break;
        case 'i':
            E.mode = MODE_INSERT;
            break;
        case 'h':
        case 'j':
        case 'k':
        case 'l':
            moveCursor(c);
            break;
        case 'x':
            insertChar(' ');
            break;
        case 'q':
            {
                ssize_t __attribute__((unused)) ret;
                int next = readKey();
                if (next == 'q') {
                    ret = write(STDOUT_FILENO, "\x1b[2J", 4);
                    ret = write(STDOUT_FILENO, "\x1b[H", 3);
                    munmap(E.mapped, E.file_size);
                    close(E.fd);
                    exit(0);
                }
            }
            break;
    }
}

int main(int argc, char *argv[]) {

    #define REFRESH_INTERVAL 100000 //microseconds
    long elapsed_time;
    struct timeval start, end;
    gettimeofday(&start, NULL);

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <filename> <offset> <size>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    size_t offset = strtoul(argv[2], NULL, 0);

    E.fd = open(filename, O_RDWR);
    if (E.fd == -1) die("open");

    E.file_size = strtoul(argv[3], NULL, 0);;

    struct stat sb;
    if (fstat(E.fd, &sb) == -1) die("fstat");

/*    if ((size_t)sb.st_size < E.offset) {
        fprintf(stderr, "Error: offset 0x%zx is beyond file size %ld\n",
                E.offset, sb.st_size);
        close(E.fd);
        return 1;
    }
*/

    E.mapped = mmap(NULL, E.file_size, PROT_READ | PROT_WRITE, MAP_SHARED, E.fd, offset);
    if (E.mapped == MAP_FAILED) die("mmap");

    E.cursor_x = 0;
    E.cursor_y = 0;
    E.col_offset = 0;
    E.row_offset = 0;
    E.screen_row_offset = 0;
    E.mode = MODE_NORMAL;
    E.command_len = 0;
    E.status_msg[0] = '\0';
    E.offset = 0;

    E.content_snapshot = malloc(E.file_size);
    if (!E.content_snapshot) die("malloc");
    memcpy(E.content_snapshot, E.mapped, E.file_size);

    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) die("getWindowSize");
    enableRawMode();
    refreshScreen();

    while (1) {
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 50000;

        int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);

        if (ret > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            int c = readKey();
        if (c != -1) {
            int old_cursor_x = E.cursor_x;
            int old_cursor_y = E.cursor_y;
            int old_mode = E.mode;
            size_t old_screen_row_offset = E.screen_row_offset;
            size_t old_col_offset = E.col_offset;

            if (E.mode == MODE_COMMAND) {
                if (c == '\r' || c == '\n') {
                    processCommand();
                } else if (c == '\x1b') {
                    E.mode = MODE_NORMAL;
                    E.command_len = 0;
                    E.command_buf[0] = '\0';
                } else if (c == 127 || c == '\b') {
                    if (E.command_len > 0) {
                        E.command_len--;
                        E.command_buf[E.command_len] = '\0';
                    } else {
                        E.mode = MODE_NORMAL;
                    }
                } else if (isprint(c)) {
                    if (E.command_len < (int)sizeof(E.command_buf) - 1) {
                        E.command_buf[E.command_len++] = c;
                        E.command_buf[E.command_len] = '\0';
                    }
                }
            } else if (E.mode == MODE_INSERT) {
                if (c == '\x1b') {
                    E.mode = MODE_NORMAL;
                } else if (c == 127 || c == '\b') {
                    if (E.cursor_x > 0) {
                        E.cursor_x--;
                    }
                } else if (c == '\r') {
                    insertChar('\n');
                } else if (isprint(c)) {
                    insertChar(c);
                }
            } else {
                switch (c) {
                    case ':':
                        E.mode = MODE_COMMAND;
                        E.command_len = 0;
                        E.command_buf[0] = '\0';
                        break;
                    case 'i':
                        E.mode = MODE_INSERT;
                        break;
                    case 'h':
                    case 'j':
                    case 'k':
                    case 'l':
                        moveCursor(c);
                        break;
                    case 'x':
                        insertChar(' ');
                        break;
                    case 'q':
                        {
                            ssize_t __attribute__((unused)) ret;
                            int next = readKey();
                            if (next == 'q') {
                                ret = write(STDOUT_FILENO, "\x1b[2J", 4);
                                ret = write(STDOUT_FILENO, "\x1b[H", 3);
                                munmap(E.mapped, E.file_size);
                                close(E.fd);
                                exit(0);
                            }
                        }
                        break;
                }
            }

            if (old_cursor_x != (int)E.cursor_x || old_cursor_y != (int)E.cursor_y ||
                old_mode != (int)E.mode || old_screen_row_offset != E.screen_row_offset ||
                old_col_offset != E.col_offset || E.command_len > 0) {
                refreshScreen();
                updateSnapshot();
            }
        }
        } else {
	    gettimeofday(&end, NULL);
	    elapsed_time = (end.tv_sec - start.tv_sec) * 1000000L + (end.tv_usec - start.tv_usec);
            if ( elapsed_time > REFRESH_INTERVAL && hasContentChanged()) {
                refreshScreen();
                updateSnapshot();
		gettimeofday(&start, NULL);
            }
        }
    }

    return 0;
}
