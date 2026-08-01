#[derive(Default)]
pub struct Input {
    pub buf: String,
    pub cursor: usize,
}

impl Input {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn is_empty(&self) -> bool {
        self.buf.is_empty()
    }

    pub fn insert_char(&mut self, c: char) {
        self.buf.insert(self.cursor, c);
        self.cursor += c.len_utf8();
    }

    pub fn insert_text(&mut self, text: &str) {
        self.buf.insert_str(self.cursor, text);
        self.cursor += text.len();
    }

    fn prev_boundary(&self) -> Option<usize> {
        self.buf[..self.cursor]
            .char_indices()
            .next_back()
            .map(|(i, _)| i)
    }

    fn next_boundary(&self) -> Option<usize> {
        self.buf[self.cursor..]
            .chars()
            .next()
            .map(|c| self.cursor + c.len_utf8())
    }

    pub fn backspace(&mut self) {
        if let Some(prev) = self.prev_boundary() {
            self.buf.remove(prev);
            self.cursor = prev;
        }
    }

    pub fn delete(&mut self) {
        if let Some(next) = self.next_boundary() {
            self.buf.drain(self.cursor..next);
        }
    }

    pub fn move_left(&mut self) {
        if let Some(prev) = self.prev_boundary() {
            self.cursor = prev;
        }
    }

    pub fn move_right(&mut self) {
        if let Some(next) = self.next_boundary() {
            self.cursor = next;
        }
    }

    pub fn move_home(&mut self) {
        self.cursor = 0;
    }

    pub fn move_end(&mut self) {
        self.cursor = self.buf.len();
    }

    pub fn move_word_left(&mut self) {
        let before = &self.buf[..self.cursor];
        let n = before.chars().count();
        let mut i = n;
        while i > 0 && before.chars().nth(i - 1).unwrap().is_whitespace() {
            i -= 1;
        }
        while i > 0 && !before.chars().nth(i - 1).unwrap().is_whitespace() {
            i -= 1;
        }
        self.cursor = before.chars().take(i).map(|c| c.len_utf8()).sum();
    }

    pub fn move_word_right(&mut self) {
        let after = &self.buf[self.cursor..];
        let chars: Vec<char> = after.chars().collect();
        let mut i = 0;
        while i < chars.len() && chars[i].is_whitespace() {
            i += 1;
        }
        while i < chars.len() && !chars[i].is_whitespace() {
            i += 1;
        }
        self.cursor += chars[..i].iter().map(|c| c.len_utf8()).sum::<usize>();
    }

    pub fn kill_to_end(&mut self) {
        self.buf.truncate(self.cursor);
    }

    pub fn kill_to_start(&mut self) {
        self.buf.drain(..self.cursor);
        self.cursor = 0;
    }

    pub fn kill_word_before(&mut self) {
        let old = self.cursor;
        self.move_word_left();
        let start = self.cursor;
        self.cursor = old;
        self.buf.drain(start..old);
        self.cursor = start;
    }

    pub fn clear(&mut self) {
        self.buf.clear();
        self.cursor = 0;
    }

    pub fn set(&mut self, text: &str) {
        self.buf = text.to_string();
        self.cursor = self.buf.len();
    }

    pub fn line_count(&self) -> usize {
        self.buf.chars().filter(|&c| c == '\n').count() + 1
    }
}
