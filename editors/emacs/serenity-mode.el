;;; serenity-mode.el --- Major mode for the Serenity language -*- lexical-binding: t; -*-

;; Copyright (C) 2026
;; SPDX-License-Identifier: MIT

;; Author: Serenity language contributors
;; Version: 0.2.0
;; Keywords: languages, serenity

;;; Commentary:
;; A small major mode for the Serenity programming language.
;;
;; Features:
;; - Syntax highlighting for the current Serenity language specification.
;; - // and /* ... */ comments.
;; - Both single-quoted and double-quoted strings.
;; - Basic brace-aware indentation.
;; - Automatic association with .sn and .snh files.
;; - Highlighting for generic/runtime types, standard stream handles, and I/O functions.
;;
;; To use this file, put it somewhere on your Emacs `load-path` and add:
;;
;;   (require 'serenity-mode)
;;
;; Or load it directly with `load-file`.

;;; Code:

(require 'font-lock)
(require 'prog-mode)

(defgroup serenity nil
  "Editing support for the Serenity programming language."
  :group 'languages
  :prefix "serenity-")

(defcustom serenity-indent-offset 4
  "Number of columns used for each Serenity indentation level."
  :type 'integer
  :group 'serenity)

(defconst serenity-mode-syntax-table
  (let ((table (make-syntax-table)))
    ;; C-style comments: // line comments and /* ... */ block comments.
    (modify-syntax-entry ?/ ". 124b" table)
    (modify-syntax-entry ?* ". 23" table)
    (modify-syntax-entry ?\n "> b" table)

    ;; Serenity strings may use either quote character.
    (modify-syntax-entry ?\" "\"" table)
    (modify-syntax-entry ?' "\"" table)
    (modify-syntax-entry ?\\ "\\" table)

    ;; Underscores are valid identifier characters.
    (modify-syntax-entry ?_ "w" table)
    table)
  "Syntax table used in `serenity-mode`.")

(defconst serenity--keywords
  '("mut" "func" "return"
    "if" "else" "while" "for" "break" "continue"
    "struct" "enum")
  "Serenity control-flow and declaration keywords.")

(defconst serenity--types
  '("int" "float" "double" "bool" "string" "any" "stream" "void")
  "Serenity primitive and runtime type names.")

(defconst serenity--constants
  '("true" "false" "null" "stdout" "stdin" "stderr")
  "Serenity literal constants and standard stream handles.")

(defconst serenity--operators
  (regexp-opt '("%%=" "=/=" "=>" "==" "<=" ">=" "<<" ">>"
                "&&" "||" "+=" "-=" "*=" "/=" "%%"
                "+" "-" "*" "/" "=" "<" ">"
                "&" "|" "^" "~" "!" ".") t)
  "Regular expression matching Serenity operators.")

(defconst serenity-font-lock-keywords
  (let ((keyword-regexp
         (regexp-opt serenity--keywords 'words))
        (type-regexp
         (regexp-opt serenity--types 'words))
        (constant-regexp
         (regexp-opt serenity--constants 'words)))
    `(
      ;; Preprocessor directives are expanded before lexing.
      ("^[ \t]*\\(%include\\)\\_>" 1 font-lock-preprocessor-face)

      ;; Function names in the explicit `func name(...)` form.
      ("\\_<func[ \t]+\\([[:alpha:]_][[:alnum:]_]*\\)\\_>"
       1 font-lock-function-name-face)

      ;; Language keywords and primitive types.
      (,keyword-regexp . font-lock-keyword-face)
      (,type-regexp . font-lock-type-face)
      (,constant-regexp . font-lock-constant-face)

      ;; Runtime I/O primitives and std/io.snh convenience functions.
      ;; These are ordinary callable names, but giving them a distinct face
      ;; makes the built-in I/O surface easy to spot while editing.
      ("\\_<\\(read\\|write\\|print\\|println\\|input\\|error\\)\\_>"
       1 font-lock-builtin-face)

      ;; Function calls, including signature-based implementations.
      ;; Earlier keyword/builtin matches take precedence.
      ("\\_<\\([[:alpha:]_][[:alnum:]_]*\\)\\_>[ \t]*("
       (1 font-lock-function-name-face keep))

      ;; Decimal integer and decimal floating-point literals.
      ("\\_<[0-9]+\\(?:\\.[0-9]+\\)?\\_>"
       . font-lock-constant-face)

      ;; The identifier following a struct/enum body is its type name:
      ;;     } Person;
      ("}[ \t\n]*\\([[:alpha:]_][[:alnum:]_]*\\)[ \t]*;"
       1 font-lock-type-face)

      ;; Field names in designated initializers and member expressions.
      ("\\.\\([[:alpha:]_][[:alnum:]_]*\\)"
       1 font-lock-variable-name-face)

      ;; Serenity operators, including the custom =/= and %% spellings.
      (,serenity--operators . font-lock-builtin-face)))
  "Font-lock rules for Serenity.")

(defun serenity--line-starts-with-closing-brace-p ()
  "Return non-nil when the current line begins with a closing brace."
  (save-excursion
    (back-to-indentation)
    (eq (char-after) ?})))

(defun serenity--calculate-indentation ()
  "Calculate indentation for the current line."
  (save-excursion
    (back-to-indentation)
    (let* ((state (syntax-ppss))
           (openers (nth 9 state))
           (brace-depth 0))
      ;; `syntax-ppss` records all currently open delimiters. Count only
      ;; braces so parentheses and brackets do not affect block indentation.
      (dolist (position openers)
        (when (eq (char-after position) ?{)
          (setq brace-depth (1+ brace-depth))))
      ;; A line beginning with } aligns with the block containing its {.
      (when (serenity--line-starts-with-closing-brace-p)
        (setq brace-depth (max 0 (1- brace-depth))))
      (* serenity-indent-offset brace-depth))))

(defun serenity-indent-line ()
  "Indent the current line according to Serenity brace structure."
  (interactive)
  (let ((indent (serenity--calculate-indentation))
        (old-column (current-column))
        (old-indent (current-indentation)))
    (indent-line-to indent)
    ;; Preserve the cursor's offset from the first non-whitespace character.
    (when (> old-column old-indent)
      (move-to-column (+ indent (- old-column old-indent))))))

;;;###autoload
(define-derived-mode serenity-mode prog-mode "Serenity"
  "Major mode for editing Serenity source and header files.

\<serenity-mode-map>
\[indent-for-tab-command] indents the current line."
  :syntax-table serenity-mode-syntax-table
  (setq-local font-lock-defaults '(serenity-font-lock-keywords))
  (setq-local indent-line-function #'serenity-indent-line)
  (setq-local comment-start "// ")
  (setq-local comment-end "")
  (setq-local comment-start-skip "\\(?://+\\|/\\*+\\)\\s-*")
  (setq-local comment-use-syntax t)
  (setq-local indent-tabs-mode nil)
  (setq-local tab-width serenity-indent-offset)
  (setq-local electric-indent-chars '(?\n ?} ?;))
  (setq-local imenu-generic-expression
              '(("Functions"
                 "^[ \t]*func[ \t]+\\([[:alpha:]_][[:alnum:]_]*\\)[ \t]*("
                 1)
                ("Signature implementations"
                 "^[ \t]*\\([[:alpha:]_][[:alnum:]_]*\\)[ \t]*(.*=>.*{"
                 1))))

;;;###autoload
(add-to-list 'auto-mode-alist '("\\.sn\\'" . serenity-mode))
;;;###autoload
(add-to-list 'auto-mode-alist '("\\.snh\\'" . serenity-mode))

(provide 'serenity-mode)

;;; serenity-mode.el ends here
