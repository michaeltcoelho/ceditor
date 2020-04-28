#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define BUFFER_INIT {NULL, 0}
#define KILO_VERSION "0.0.1"

typedef struct editorRow {
    int size;
    char *chars;
} editorRow;

struct editorConfig {
    int cx, cy; // cursor coordenates
    int screenrows;
    int screencols;
    int numRows;
    int rowOffset;
    editorRow *row;
    struct termios originalTermAttrs;
};

struct editorConfig state;

enum moveKeys {
    MOVE_LEFT  = 'h',
    MOVE_RIGHT = 'l',
    MOVE_UP    = 'k',
    MOVE_DOWN  = 'j'
};

struct buffer {
    char *b;
    int len;
};

void clearEditorScreen();

void appendToBuffer(struct buffer *buf, const char *s, int len) {
    char *new = realloc(buf->b, buf->len + len);

    if (new == NULL) return;
    memcpy(&new[buf->len], s, len);
    buf->b = new;
    buf->len += len;
}

void freeBuffer(struct buffer *buf) {
    free(buf->b);
}

void die(const char *s) {
    clearEditorScreen();
    perror(s);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &state.originalTermAttrs) == -1) die("tcsetattr");
}

// enable raw mode
void enableCustomTerminalMode() {
    // read terminal settings into "originalTermAttrs"
    if(tcgetattr(STDIN_FILENO, &state.originalTermAttrs) == -1) die("tcgetattr");

    // rollback terminal changes when program exits
    atexit(disableRawMode);

    struct termios termAttrs = state.originalTermAttrs;
    // disable ctrl+s/ctrl+q freezing data from being transmitted to the terminal
    termAttrs.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // disable output processing like: `\n` to `\r\n`. `\r` carriage return, it moves cursor to the begining of the current line
    termAttrs.c_oflag &= ~(OPOST);
    // disable miscellaneous flags
    termAttrs.c_cflag |= (CS8);
    // disable echoing mode, canonical mode and signint and also sigtstp
    termAttrs.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // read() returns if it doesnt get any input for a certain amount of time
    termAttrs.c_cc[VMIN] = 0; // min. number of bytes input needed before read() can return
    termAttrs.c_cc[VTIME] = 1; // max. amount of time to wait before read() returns

    // apply changes to the terminal
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &termAttrs) == -1) die("tcsetattr");
}

char readKeysFromInput() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        // read and map arrow keys to keys defined in moveKeys
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return MOVE_UP;
                case 'B': return MOVE_DOWN;
                case 'C': return MOVE_RIGHT;
                case 'D': return MOVE_LEFT;
            }
        }
        return '\x1b';

    } else {
        return c;
    }
}


void editorMoveCursor(char key) {
    switch (key) {
        case MOVE_LEFT:
            if (state.cx != 0)
                state.cx--;
            break;
        case MOVE_RIGHT:
            if (state.cx != state.screencols - 1)
                state.cx++;
            break;
        case MOVE_UP:
            if (state.cy != 0)
                state.cy--;
            break;
        case MOVE_DOWN:
            if (state.cy < state.numRows)
                state.cy++;
            break;
    }
}

void mapEditorKeys() {
    char c = readKeysFromInput();
    switch (c) {
        case CTRL_KEY('q'):
            clearEditorScreen();
            exit(0);
            break;

        case MOVE_LEFT:
        case MOVE_RIGHT:
        case MOVE_UP:
        case MOVE_DOWN:
            editorMoveCursor(c);
            break;
    }
}

// read cursor position by sending status report char escape to VT100
int getCursorPosition(int *rows, int *cols) {
    // send cursor to right bottom of the screen
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    // write out status report escape char
    if (write(STDOUT_FILENO, "\x1b[6n]", 4) != 4) return -1;

    char buffer[32];
    unsigned int i = 0;

    while (i < sizeof(buffer) - 1) {
        if (read(STDIN_FILENO, &buffer[i], 1) != 1) break;
        if (buffer[i] == 'R') break;
        i++;
    }

    buffer[i] = '\0';

    if (buffer[0] != '\x1b' || buffer[1] != '[') return -1;
    if (sscanf(&buffer[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)  {
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void editorAppendRow(char *s, size_t len) {
    state.row = realloc(state.row, sizeof(editorRow) * (state.numRows + 1));

    int at = state.numRows;

    state.row[at].size = len;
    state.row[at].chars = malloc(len+ 1);
    memcpy(state.row[at].chars, s, len);
    state.row[at].chars[len] = '\0';
    state.numRows++;
}

// open a file and read it to memory
void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t lineCap = 0;

    ssize_t lineLen;

    while ((lineLen = getline(&line, &lineCap, fp)) != -1) {
        while (lineLen > 0 && (line[lineLen - 1] == '\n' || line[lineLen] == '\r'))
            lineLen--;
        editorAppendRow(line, lineLen);
    }

    free(line);
    fclose(fp);
}

int drawWelcomeMessage(int *row, struct buffer *buf) {
    if (state.numRows == 0 && *row == state.screenrows / 3) {
        char message[80];
        int messageLen = snprintf(message, sizeof(message), "Kilo editor -- version %s", KILO_VERSION);
        // truncate message in case screen is too tiny to fit message
        if (messageLen > state.screencols)
            messageLen = state.screencols;
        // padding
        int padding = (state.screencols - messageLen) / 2;
        if (padding) {
            appendToBuffer(buf, "~", 1);
            padding--;
        }
        while (padding--) appendToBuffer(buf, " ", 1);
        appendToBuffer(buf, message, messageLen);
        return 0;
    } else {
        return -1;
    }
}

void editorScroll() {
    if (state.cy < state.rowOffset)
        state.rowOffset = state.cy;
    if (state.cy >= state.rowOffset + state.screenrows)
        state.rowOffset = state.cy - state.screenrows + 1;
}

void drawRows(struct buffer *buf) {
    int y;
    for (y = 0; y < state.screenrows; y++) {

        int fileRow = y + state.rowOffset;

        if (fileRow >= state.numRows) {
            if (drawWelcomeMessage(&y, buf) == -1) {
                appendToBuffer(buf, "~", 1);
            }
        } else {
            int len = state.row[fileRow].size;
            if (len > state.screencols) len = state.screencols;
            appendToBuffer(buf, state.row[fileRow].chars, len);
        }
        // clear lines as we write out new ones
        appendToBuffer(buf, "\x1b[K", 3);

        if (y < state.screenrows - 1) {
            appendToBuffer(buf, "\r\n", 2);
        }
    }
}

void clearEditorScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

void refreshEditorScreen() {
    editorScroll();

    struct buffer buf = BUFFER_INIT;

    appendToBuffer(&buf, "\x1b[?25l", 6);
    appendToBuffer(&buf, "\x1b[H", 3);

    drawRows(&buf);

    char cursorBuf[32];
    snprintf(cursorBuf, sizeof(cursorBuf), "\x1b[%d;%dH", (state.cy - state.rowOffset) + 1, state.cx + 1);
    appendToBuffer(&buf, cursorBuf, strlen(cursorBuf));

    //appendToBuffer(&buf, "\x1b[H", 3);
    appendToBuffer(&buf, "\x1b[?25h", 6);

    write(STDOUT_FILENO, buf.b, buf.len);
    freeBuffer(&buf);
}

// initalize editor global state
void initEditor() {
    // initialy cursor coordenates
    state.cx = 0;
    state.cy = 0;
    state.row = NULL;
    state.rowOffset = 0;
    state.numRows = 0;

    if (getWindowSize(&state.screenrows, &state.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableCustomTerminalMode();
    initEditor();

    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (1) {
        refreshEditorScreen();
        mapEditorKeys();
    }

    return 0;
}
