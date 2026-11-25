# Tool inference + shell completion demo GIF, recorded with VHS:
#   brew install vhs
#   make                                     # builds ./nls; the tape uses this build
#   sh marketing/make-demo-media.sh          # creates marketing/demo/ (file names for Tab)
#   vhs marketing/infer-completion.tape      # writes docs/nls-infer-completion.gif
# Run from the repo root. Nothing is executed: nls only prints commands.

Output docs/nls-infer-completion.gif

Set Shell zsh
Set FontSize 20
Set Width 1300
Set Height 700
Set Padding 30
Set TypingSpeed 45ms
Set Theme "Catppuccin Mocha"

Hide
# the repo's build and completions, not an installed nls; the Ctrl+G widget
Type `export PATH="$PWD:$PATH" && fpath=($PWD/completions $fpath) && autoload -Uz compinit && compinit -u && zstyle ':completion:*' menu select && eval "$(nls --shell zsh)" && cd marketing/demo && clear`
Enter
Show

# 1. No tool named: nls picks the one that fits
Type `nls "convert demo.mov to gif"`
Sleep 400ms
Enter
Sleep 2.5s

Type `nls "count the items in data.json"`
Sleep 400ms
Enter
Sleep 2s

Type `nls "extract backup.tar.gz into restore/"`
Sleep 400ms
Enter
Sleep 2s

Type `nls "undo my last commit"`
Sleep 400ms
Enter
Sleep 2s

Type `nls "check if https://example.com is up"`
Sleep 400ms
Enter
Sleep 3s

# 2. A close call is flagged; nothing to go on is refused
Hide
Type "clear"
Enter
Show

Type `nls "find TODO in src/"`
Sleep 400ms
Enter
Sleep 3.5s

Type `nls "go back to main"`
Sleep 400ms
Enter
Sleep 3s

Type `nls "rsync src/ to backup/"`
Sleep 400ms
Enter
Sleep 3s

# 3. -v shows every tool's score
Hide
Type "clear"
Enter
Show

Type `nls -v "shrink recording.webm"`
Sleep 400ms
Enter
Sleep 5s

# 4. Tab completion: tools, options, --shell values
Hide
Type "clear"
Enter
Show

Type "nls "
Sleep 300ms
Tab
Sleep 2.5s
Ctrl+C
Sleep 500ms

Type "nls g"
Sleep 300ms
Tab
Sleep 2.5s
Ctrl+C
Sleep 500ms

Type "nls --e"
Sleep 300ms
Tab
Sleep 800ms
Tab
Sleep 2.5s
Ctrl+C
Sleep 500ms

Type "nls --shell "
Sleep 300ms
Tab
Sleep 2.5s
Ctrl+C
Sleep 500ms

# 5. Ctrl+G: the typed request becomes the command, ready to edit
Hide
Type "clear"
Enter
Show

Type "cut input.mp4 from 00:10 to 00:25"
Sleep 800ms
Ctrl+G
Sleep 3s
Ctrl+C
Sleep 500ms

Type "make thumb.jpg 300 pixels wide"
Sleep 800ms
Ctrl+G
Sleep 3s
Ctrl+C
Sleep 500ms

Type "find TODO in src/"
Sleep 800ms
Ctrl+G
Sleep 4s
