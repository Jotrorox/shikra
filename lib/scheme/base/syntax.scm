;; Syntax. Every core and derived form is implemented by the expander (src/expand.c);
;; this file only re-exports the keywords of (shikra primitives). parameterize and guard
;; are syntax-rules macros over library procedures (control.scm, errors.scm).
(export define lambda if quote set! begin
        let let* letrec letrec* cond case and or when unless do
        quasiquote unquote unquote-splicing
        define-syntax let-syntax letrec-syntax syntax-rules syntax-error
        define-record-type let-values let*-values define-values
        include include-ci cond-expand
        else => ... _)
