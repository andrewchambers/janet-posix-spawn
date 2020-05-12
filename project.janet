(declare-project
  :name "posix-spawn"
  :author "Andrew Chambers"
  :license "MIT"
  :url "https://github.com/andrewchambers/janet-posix-spawn"
  :repo "git+https://github.com/andrewchambers/janet-posix-spawn.git")

(declare-native
  :name "_jmod_posix_spawn"
  :source ["posix-spawn.c"])

(declare-source
  :source ["posix-spawn.janet"])

