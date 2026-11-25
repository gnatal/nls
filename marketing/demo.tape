# Terminal demo GIF, recorded with VHS: https://github.com/charmbracelet/vhs
#   brew install vhs
#   make                                 # builds ./nls; the tape uses this build
#   sh marketing/make-demo-media.sh      # creates marketing/demo/ (talk.mp4, users.json, src/, app.log, logs/, assets/, node_modules/, release-1.0.tgz, dist/, project/ git repo, data.json, hero.png, sprite.png, frames/)
#   vhs marketing/demo.tape              # writes marketing/nls-demo.gif
# Run from the repo root.

Output marketing/nls-demo.gif

Set Shell zsh
Set FontSize 20
Set Width 1300
Set Height 620
Set Padding 30
Set TypingSpeed 45ms
Set Theme "Catppuccin Mocha"

Hide
# use the repo's build, not an older installed nls; keep macOS tar from adding ._ files
Type `export PATH="$PWD:$PATH" COPYFILE_DISABLE=1 && cd marketing/demo && clear`
Enter
Show

# 1. The ffmpeg flags nobody remembers
Type `nls ffmpeg "keep the first 10 seconds of talk.mp4 into intro.mp4"`
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

# 5. find: ages, sizes, pruning, and the cleanup incantation nobody remembers
Hide
Type "clear"
Enter
Show

Type `nls -x -y find "log files older than 30 days"`
Sleep 400ms
Enter
Sleep 2.5s

Type `nls -x -y find "files larger than 5MB ignoring node_modules"`
Sleep 400ms
Enter
Sleep 3s

Type `nls find "delete all node_modules directories"`
Sleep 400ms
Enter
Sleep 3.5s

# 6. tar: excludes, compression from the name, --strip-components
Hide
Type "clear"
Enter
Show

Type `nls -x -y tar "back up src/ into backup.tgz, skipping .git, node_modules and .DS_Store"`
Sleep 400ms
Enter
Sleep 3s

Type `nls -x -y tar "extract release-1.0.tgz into dist/ without the top-level folder"`
Sleep 400ms
Enter
Sleep 3.5s

# 7. git: history search, blame ranges, branch comparison, squash
Hide
Type "cd project && clear"
Enter
Show

Type `nls -x -y git "find commit that added 'def retry'"`
Sleep 400ms
Enter
Sleep 2.5s

Type `nls -x -y git "who changed lines 3 to 4 of src/server.py"`
Sleep 400ms
Enter
Sleep 3s

Type `nls -x -y git "commits on feature-login not on main"`
Sleep 400ms
Enter
Sleep 2.5s

Type `nls git "squash last 3 commits with message 'login flow'"`
Sleep 400ms
Enter
Sleep 3.5s

# 8. curl: the JSON POST incantation, a live status check, tokens kept as variables
Hide
Type "cd .. && clear"
Enter
Show

Type `nls curl "post data.json to https://api.example.com/users"`
Sleep 400ms
Enter
Sleep 3s

Type `nls -x -y curl "check response status of https://example.com"`
Sleep 400ms
Enter
Sleep 3s

Type `nls curl 'delete https://api.example.com/users/42 with token $TOKEN'`
Sleep 400ms
Enter
Sleep 3.5s

# 9. magick: resize + format + quality in one sentence, background removal, a GIF from frames
Hide
Type "clear"
Enter
Show

Type `nls -x -y magick "resize hero.png to width 800 convert to webp quality 85"`
Sleep 400ms
Enter
Sleep 2.5s

Type `nls -x -y magick "what are the dimensions of hero.webp"`
Sleep 400ms
Enter
Sleep 2.5s

Type `nls magick "make sprite.png transparent background"`
Sleep 400ms
Enter
Sleep 3s

Type `nls magick "make a gif from frames/*.png at 12 fps"`
Sleep 400ms
Enter
Sleep 3.5s
