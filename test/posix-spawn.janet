(use ../posix-spawn)

(with [f1 (file/temp)]
(with [f2 (file/temp)]
  (run ["echo" "hello"] :file-actions [[:dup2 f1 stdout][:close f2]])
  (file/seek f1 :set 0)
  (assert (deep= (file/read f1 :all) @"hello\n"))))

(with [f (file/temp)]
  (run ["env"] :file-actions [[:dup2 f stdout]] :env {"XXXXXXXXXXX" "POSIX_SPAWN_NEEDLE"})
  (file/seek f :set 0)
  (def out (string (file/read f :all) "hello\n"))
  (assert (string/find "POSIX_SPAWN_NEEDLE" out)))
