#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>

#define CTRL_KEY(k) ((k) & 0x1f)

struct editorConfig {
    int screenrows;
    int screencols;
    struct termios originalTermAttrs;
};

struct editorConfig state;

struct buffer {
    char *b;
    int len;
};

#define BUFFER_INIT {NULL, 0}

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
    return c;
}

void mapEditorKeys() {
    char c = readKeysFromInput();
    switch (c) {
        case CTRL_KEY('q'):
            clearEditorScreen();
            exit(0);
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

void drawTildesRows(struct buffer *buf) {
    int y;
    for (y = 0; y < state.screenrows; y++) {
        appendToBuffer(buf, "~", 1);

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
    struct buffer buf = BUFFER_INIT;

    appendToBuffer(&buf, "\x1b[?25l", 6);
    appendToBuffer(&buf, "\x1b[H", 3);

    drawTildesRows(&buf);

    appendToBuffer(&buf, "\x1b[H", 3);
    appendToBuffer(&buf, "\x1b[?25h", 6);

    write(STDOUT_FILENO, buf.b, buf.len);
    freeBuffer(&buf);
}

// initalize editor global state
void initEditor() {
    if (getWindowSize(&state.screenrows, &state.screencols) == -1) die("getWindowSize");
}

int main() {
    enableCustomTerminalMode();
    initEditor();

    while (1) {
        refreshEditorScreen();
        mapEditorKeys();
    }

    return 0;
}
