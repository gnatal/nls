# Terminal demo GIF, recorded with VHS: https://github.com/charmbracelet/vhs
#   brew install vhs
#   sh marketing/make-demo-media.sh      # creates marketing/demo/ (talk.mp4, users.json, src/, app.log)
#   vhs marketing/demo.tape              # writes marketing/nls-demo.gif
# Requires nls on PATH (make install).

Output marketing/nls-demo.gif

Set Shell zsh
Set FontSize 20
Set Width 1300
Set Height 620
Set Padding 30
Set TypingSpeed 45ms
Set Theme "Catppuccin Mocha"

Hide
Type "cd marketing/demo && clear"
Enter
Show

# 1. The ffmpeg flags nobody remembers
Type `nls ffmpeg "extract from talk.mp4 the first 10 seconds into intro.mp4"`
Sleep 400ms
Enter
Sleep 2.5s

# 2. A GIF, palette-optimized
Type `nls ffmpeg "make a gif from demo.mp4 between 2 and 5 seconds"`
Sleep 400ms
Enter
Sleep 3s

# 3. jq that understands the file's fields
Type `nls -x -y jq "in users.json how many users have emails"`
Sleep 400ms
Enter
Sleep 2.5s

Type `nls -x -y jq "get names from users.json where age > 30"`
Sleep 400ms
Enter
Sleep 3.5s

# 4. grep: file types, case, context
Hide
Type "clear"
Enter
Show

Type `nls -x -y grep "which python files contain TODO, ignoring case"`
Sleep 400ms
Enter
Sleep 2.5s

Type `nls -x -y grep "find ERROR in app.log with 1 line of context"`
Sleep 400ms
Enter
Sleep 3.5s
