;; mus-mode.el --- Major mode for the Mus language
;; this file is rewritten from https://github.com/rexim/simpc-mode

(require 'subr-x)

(defvar mus-mode-syntax-table
  (let ((table (make-syntax-table)))
    ;; comments
    (modify-syntax-entry ?# "<" table)
    (modify-syntax-entry ?\n ">" table)
    ;; Chars are the same as strings
    (modify-syntax-entry ?' "\"" table)
    
    (modify-syntax-entry ?& "." table)
    (modify-syntax-entry ?% "." table)
    table))

(defun mus-types ()
  '("void" "bool"
    "u8" "u16" "u32" "u64"
    "i8" "i16" "i32" "i64"))

(defun mus-keywords ()
  '("fn" "var" "extern" "if" "else"))

(defun mus-font-lock-keywords ()
  (list
   `(,(regexp-opt (mus-keywords) 'symbols) . font-lock-keyword-face)
   `(,(regexp-opt (mus-types) 'symbols) . font-lock-type-face)))

(defun mus--previous-non-empty-line ()
  "Returns either NIL when there is no such line or a pair (line . indentation)"
  (save-excursion
    ;; If you are on the first line, but not at the beginning of buffer (BOB) the `(bobp)`
    ;; function does not return `t`. So we have to move to the beginning of the line first.
    ;; TODO: feel free to suggest a better approach for checking BOB here.
    (move-beginning-of-line nil)
    (if (bobp)
        ;; If you are standing at the BOB, you by definition don't have a previous non-empty line.
        nil
      ;; Moving one line backwards because the current line is by definition is not
      ;; the previous non-empty line.
      (forward-line -1)
      ;; Keep moving backwards until we hit BOB or a non-empty line.
      (while (and (not (bobp))
                  (string-empty-p
                   (string-trim-right
                    (thing-at-point 'line t))))
        (forward-line -1))

      (if (string-empty-p
           (string-trim-right
            (thing-at-point 'line t)))
          ;; If after moving backwards for this long we still look at an empty
          ;; line we by definition didn't find the previous non-empty line.
          nil
        ;; We found the previous non-empty line!
        (cons (thing-at-point 'line t)
              (current-indentation))))))

(defun mus--desired-indentation ()
  (let ((prev (mus--previous-non-empty-line)))
    (if (not prev)
        (current-indentation)
      (let ((indent-len 4)
            (cur-line (string-trim-right (thing-at-point 'line t)))
            (prev-line (string-trim-right (car prev)))
            (prev-indent (cdr prev)))
        (cond
         ((string-match-p "^\\s-*switch\\s-*(.+)" prev-line)
          prev-indent)
         ((and (string-suffix-p "{" prev-line)
               (string-prefix-p "}" (string-trim-left cur-line)))
          prev-indent)
         ((string-suffix-p "{" prev-line)
          (+ prev-indent indent-len))
         ((string-prefix-p "}" (string-trim-left cur-line))
          (max (- prev-indent indent-len) 0))
         ((string-suffix-p ":" prev-line)
          (if (string-suffix-p ":" cur-line)
              prev-indent
            (+ prev-indent indent-len)))
         ((string-suffix-p ":" cur-line)
          (max (- prev-indent indent-len) 0))
         (t prev-indent))))))

;;; TODO: customizable indentation (amount of spaces, tabs, etc)
(defun mus-indent-line ()
  (interactive)
  (when (not (bobp))
    (let* ((desired-indentation
            (mus--desired-indentation))
           (n (max (- (current-column) (current-indentation)) 0)))
      (indent-line-to desired-indentation)
      (forward-char n))))

(define-derived-mode mus-mode prog-mode "Mus language"
  "Simple major mode for editing Mus files."
  :syntax-table mus-mode-syntax-table
  (setq-local font-lock-defaults '(mus-font-lock-keywords))
  (setq-local indent-line-function 'mus-indent-line)
  (setq-local comment-start "# "))

(add-to-list 'auto-mode-alist '("\\.mus\\'" . mus-mode))

(provide 'mus-mode)

