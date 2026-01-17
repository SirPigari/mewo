;;; mewo-mode.el --- Major mode for Mewo build system files -*- lexical-binding: t; -*-

;; Copyright (C) 2026 SirPigari

;; Author: SirPigari
;; Version: 1.0.0
;; Keywords: languages, tools
;; URL: https://github.com/SirPigari/mewo
;; Package-Requires: ((emacs "24.3"))

;; This file is not part of GNU Emacs.

;;; Commentary:

;; Major mode for editing Mewo build system files (Mewofile).
;; Provides syntax highlighting for:
;;   - Comments (; and //)
;;   - Labels (name:)
;;   - Attributes (#shell, #cwd, #ignorefail, etc.)
;;   - Conditionals (#if, #else, #endif, #windows, #linux, etc.)
;;   - Variable interpolation (${var}, $0, $?)
;;   - Strings, numbers, booleans, arrays
;;   - Control flow (goto, call)

;;; Installation:

;; Add to your init.el:
;;   (add-to-list 'load-path "/path/to/mewo/editor/emacs")
;;   (require 'mewo-mode)

;; Or with use-package:
;;   (use-package mewo-mode
;;     :load-path "/path/to/mewo/editor/emacs"
;;     :mode (("Mewofile\\'" . mewo-mode)
;;            ("\\.mewo\\'" . mewo-mode)))

;;; Code:

(defgroup mewo nil
  "Major mode for Mewo build system files."
  :group 'languages
  :prefix "mewo-")

(defface mewo-label-face
  '((t :inherit font-lock-function-name-face :weight bold))
  "Face for Mewo labels."
  :group 'mewo)

(defface mewo-attribute-face
  '((t :inherit font-lock-preprocessor-face))
  "Face for Mewo attributes."
  :group 'mewo)

(defface mewo-conditional-face
  '((t :inherit font-lock-keyword-face))
  "Face for Mewo conditionals."
  :group 'mewo)

(defface mewo-interpolation-face
  '((t :inherit font-lock-variable-name-face))
  "Face for Mewo variable interpolation."
  :group 'mewo)

(defface mewo-builtin-face
  '((t :inherit font-lock-builtin-face))
  "Face for Mewo built-in commands."
  :group 'mewo)

(defface mewo-escape-face
  '((t :inherit font-lock-constant-face))
  "Face for escaped interpolation."
  :group 'mewo)

(defconst mewo-mode-syntax-table
  (let ((table (make-syntax-table)))
    ;; Semicolon starts a comment
    (modify-syntax-entry ?\; "<" table)
    (modify-syntax-entry ?\n ">" table)
    ;; Strings
    (modify-syntax-entry ?\" "\"" table)
    (modify-syntax-entry ?\' "\"" table)
    ;; Brackets
    (modify-syntax-entry ?\[ "(]" table)
    (modify-syntax-entry ?\] ")[" table)
    (modify-syntax-entry ?\( "()" table)
    (modify-syntax-entry ?\) ")(" table)
    (modify-syntax-entry ?\{ "(}" table)
    (modify-syntax-entry ?\} "){" table)
    ;; Underscore is part of words
    (modify-syntax-entry ?_ "w" table)
    (modify-syntax-entry ?- "w" table)
    table)
  "Syntax table for `mewo-mode'.")

(defconst mewo-font-lock-keywords
  `(
    ;; Comments with //
    ("//.*$" . font-lock-comment-face)

    ;; Escaped interpolation $${...}
    ("\\$\\$\\{[^}]*\\}" . 'mewo-escape-face)

    ;; Variable interpolation ${...}
    ("\\${[^}]+}" . 'mewo-interpolation-face)

    ;; Positional arguments $0, $1, etc.
    ("\\$[0-9]+" . 'mewo-interpolation-face)

    ;; Exit code $?
    ("\\$\\?" . 'mewo-interpolation-face)

    ;; Labels at start of line (name:)
    ("^\\s-*\\([a-zA-Z_][a-zA-Z0-9_-]*\\)\\s-*:" 1 'mewo-label-face)

    ;; Conditionals: #if(...), #else, #endif, #exists(...)
    ("#if\\s-*([^)]*)" . 'mewo-conditional-face)
    ("#\\(else\\|endif\\)\\b" . 'mewo-conditional-face)
    ("#exists\\s-*([^)]*)" . 'mewo-conditional-face)

    ;; Platform conditionals
    ("#\\(windows\\|win32\\|linux\\|macos\\|darwin\\|unix\\)\\b" . 'mewo-conditional-face)

    ;; Attributes with parameters
    ("#\\(shell\\|cwd\\|ignorefail\\|expect\\|timeout\\|once\\|save\\|env\\|assert\\|arch\\|distro\\|feature\\)\\s-*([^)]*)" . 'mewo-attribute-face)

    ;; Attributes without parameters
    ("#\\(shell\\|ignorefail\\|once\\)\\b" . 'mewo-attribute-face)

    ;; Features
    ("#features?\\s-*([^)]*)" . 'mewo-attribute-face)

    ;; Built-in functions in interpolation
    ("#\\(defined\\|len\\|env\\)\\s-*([^)]*)" . font-lock-builtin-face)

    ;; Control flow keywords
    ("\\b\\(goto\\|call\\)\\b" . font-lock-keyword-face)

    ;; Built-in commands
    ("\\becho\\b" . 'mewo-builtin-face)

    ;; Variable assignment
    ("^\\s-*\\([a-zA-Z_][a-zA-Z0-9_]*\\)\\s-*=" 1 font-lock-variable-name-face)

    ;; Array index assignment
    ("^\\s-*\\([a-zA-Z_][a-zA-Z0-9_]*\\)\\s-*\\[[^]]+\\]\\s-*=" 1 font-lock-variable-name-face)

    ;; Booleans
    ("\\b\\(true\\|false\\)\\b" . font-lock-constant-face)

    ;; Numbers
    ("\\b-?[0-9]+\\(?:\\.[0-9]+\\)?\\b" . font-lock-constant-face)

    ;; Assignment operator
    ("=" . font-lock-keyword-face))
  "Font lock keywords for `mewo-mode'.")

(defun mewo-indent-line ()
  "Indent current line for `mewo-mode'.
Lines inside labels are indented with 4 spaces."
  (interactive)
  (let ((indent 0)
        (pos (- (point-max) (point))))
    (save-excursion
      (beginning-of-line)
      ;; Check if we're inside a label block
      (when (save-excursion
              (and (re-search-backward "^\\s-*[a-zA-Z_][a-zA-Z0-9_-]*\\s-*:" nil t)
                   (not (looking-at "^\\s-*[a-zA-Z_][a-zA-Z0-9_-]*\\s-*:"))))
        (setq indent 4)))
    (if (<= (current-column) (current-indentation))
        (indent-line-to indent)
      (save-excursion (indent-line-to indent)))
    (when (> (- (point-max) pos) (point))
      (goto-char (- (point-max) pos)))))

(defun mewo-comment-dwim (arg)
  "Comment or uncomment current line or region.
With prefix ARG, use that many semicolons."
  (interactive "*P")
  (comment-dwim arg))

(defvar mewo-mode-map
  (let ((map (make-sparse-keymap)))
    (define-key map (kbd "C-c C-c") #'mewo-comment-dwim)
    map)
  "Keymap for `mewo-mode'.")

;;;###autoload
(define-derived-mode mewo-mode prog-mode "Mewo"
  "Major mode for editing Mewo build system files.

\\{mewo-mode-map}"
  :syntax-table mewo-mode-syntax-table
  (setq-local comment-start "; ")
  (setq-local comment-end "")
  (setq-local comment-start-skip ";+\\s-*")
  (setq-local font-lock-defaults '(mewo-font-lock-keywords))
  (setq-local indent-line-function #'mewo-indent-line)
  (setq-local indent-tabs-mode nil)
  (setq-local tab-width 4))

;;;###autoload
(add-to-list 'auto-mode-alist '("Mewofile\\'" . mewo-mode))
;;;###autoload
(add-to-list 'auto-mode-alist '("\\.mewo\\'" . mewo-mode))

(provide 'mewo-mode)

;;; mewo-mode.el ends here
