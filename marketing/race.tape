# Split-screen race GIF, recorded with VHS: https://github.com/charmbracelet/vhs
# The landing page's race (#race in docs/index.html) in a terminal: the real
# nls on the left, a labelled simulation of an LLM CLI helper on the right.
#   brew install vhs
#   make                                 # builds ./nls; the race runs this build
#   sh marketing/make-demo-media.sh      # creates marketing/demo/ (users.json, src/, ...)
#   vhs marketing/race.tape              # writes marketing/nls-race.gif
# Run from the repo root. Needs node. Nothing is executed: nls only prints.

Output marketing/nls-race.gif

Set Shell zsh
Set FontSize 15
Set Width 1600
Set Height 940
Set Padding 24
Set Framerate 20
Set Theme "Catppuccin Mocha"

Hide
Type `cd marketing/demo && clear`
Enter
Show

Type `node ../race.cjs ../../nls`
Enter
# 0.4 s start, then 23.8 s of simulated waits; race.cjs holds the result
Sleep 29s
