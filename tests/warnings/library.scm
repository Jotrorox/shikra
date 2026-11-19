;; A user library: unused imports and unexported unused definitions are reported there;
;; exports the program does not use are not.
(import (scheme base) (scheme write) (wlib util))
(display (triple 3))
(newline)
