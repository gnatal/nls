# Homebrew formula for nls. Lives in a tap repo (github.com/gnatal/homebrew-tap,
# as Formula/nls.rb) so users can run:  brew install gnatal/tap/nls
# For each release, update `url` and `sha256` (see RELEASING.md).
class Nls < Formula
  desc "Turn plain-English requests into exact shell commands, fully offline"
  homepage "https://github.com/gnatal/nls"
  url "https://github.com/gnatal/nls/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "b31b4c414c6d068d6ca58022f14cd10ceed06c494fe300e7fe599e1c95900e3d"
  license "MIT"
  head "https://github.com/gnatal/nls.git", branch: "main"

  def install
    # No -Werror: a newer compiler's new warnings shouldn't break installs.
    system "make", "CC=#{ENV.cc}", "CFLAGS=-std=c11 -O2"
    bin.install "nls"
    man1.install "man/nls.1"
    bash_completion.install "completions/nls.bash" => "nls"
    zsh_completion.install "completions/_nls"
  end

  def caveats
    <<~EOS
      To turn the typed command line into a command with Ctrl+G, add to ~/.zshrc:
        eval "$(nls --shell zsh)"
      or to ~/.bashrc (bash 4.0 or later):
        eval "$(nls --shell bash)"
    EOS
  end

  test do
    assert_equal "jq . data.json", shell_output("#{bin}/nls jq 'pretty print data.json'").strip
    assert_equal "jq . data.json", shell_output("#{bin}/nls 'pretty print data.json'").strip
    assert_match version.to_s, shell_output("#{bin}/nls --version")
  end
end
