; lists, pairs, quote forms, comments
()
(a b c)
(a . b)
(1 2 . 3)
(a (b (c (d))) . e)
( spaced   out
  over lines )
(a . (b . (c . ())))
'x
`(a ,b ,@c)
''x
(quote)
#;(this is skipped) kept
(a #;b c)
#| block
   #| nested |#
   comment |# after-block
(x ; comment inside
 y)
#(1 #(2) "three" #\4)
#()
#u8()
#u8(0 1 127 128 255)
#t #f #true #false
(#t . #f)
