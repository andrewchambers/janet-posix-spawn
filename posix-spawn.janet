(import _jmod_posix_spawn :as _posix-spawn)

(def POSIX_SPAWN_SETSIGMASK _posix-spawn/POSIX_SPAWN_RESETIDS)
(def POSIX_SPAWN_SETSIGDEF _posix-spawn/POSIX_SPAWN_SETSIGDEF)
(def POSIX_SPAWN_RESETIDS _posix-spawn/POSIX_SPAWN_RESETIDS)
(def SIGINT _posix-spawn/SIGINT)
(def SIGHUP _posix-spawn/SIGHUP)
(def SIGPIPE _posix-spawn/SIGPIPE)
(def SIGTERM _posix-spawn/SIGTERM)
(def SIGKILL _posix-spawn/SIGKILL)
(def SIGUSR1 _posix-spawn/SIGUSR1)
(def SIGUSR2 _posix-spawn/SIGUSR2)

(defn spawn2
  "The same as spawn, but takes a dictionary of arguments instead of &keys style arguments."
  [args {:cmd cmd
         :close-signal close-signal
         :file-actions file-actions
         :env env
         :attr-flags attr-flags
         :sig-default sig-default
         :sig-mask sig-mask}]
  # In threaded code this copy is needed.
  # We could avoid this in single threaded janet builds.
  (default env (os/environ))
  (default cmd (get args 0))
  (default close-signal SIGTERM)
  (default sig-default :all)
  (default attr-flags
    (comptime (bor POSIX_SPAWN_SETSIGMASK POSIX_SPAWN_SETSIGDEF POSIX_SPAWN_RESETIDS)))
  (_posix-spawn/spawn cmd args close-signal file-actions env attr-flags sig-default sig-mask))

(defn spawn
`
Spawn a child process using libc posix_spawn. Care
must be taken with file descriptors that have not had the CLOEXEC flag set, as
they will be inherited by child processes.

Positional args:

args - A tuple or array of strings or symbols to be used as arguments.

Keyword args:

:cmd

The command to run, defaults to (args 0).

:close-signal

Signal to send process on when close is called. Also
called when process is garbage collected.

:file-actions
  
A tuple of file actions the child will take before calling execve.

Valid file action formats:

[:dup2 file1 file2] - Call dup2 on the specified files.
[:close file] - Close the specified files.

:env

The process environment, defaults to (os/environ)

:attr-flags

A set of spawn flags, see the posix_spawn(3) man page for details.
Defaults to POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF|POSIX_SPAWN_RESETIDS.

:sig-default

A set of signals values to reset to their default handlers.
As a special case, :all may be passed. nil
means no signals. Defaults to :all. 

:sig-mask

A set of signals to mask. As a special case, :all may be passed.
nil means no signals. Defaults to nil.
`
  [args &keys kwargs]
  (spawn2 args kwargs))

(defn wait
  "Wait for the process to exist and return the exit status."
  [p]
  (_posix-spawn/wait p))

(defn run
  "Equivalent to spawn followed by wait."
  [args &keys kwargs]
  (wait (spawn2 args kwargs)))

(defn run2
  "Equivalent to spawn2 followed by wait."
  [args kwargs]
  (wait (spawn2 args kwargs)))

(defn close 
  "Send the process it's close signal and wait for it to exit."
  [p]
  (_posix-spawn/close p))

(defn pipe
  "Create a pair of files created with pipe. The files have the CLOEXEC flag set."
  []
  (_posix-spawn/pipe))

(defn dup
  "Duplicate a file descriptor, sets the CLOEXEC flag."
  [f]
  (_posix-spawn/dup f))

