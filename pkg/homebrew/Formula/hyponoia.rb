class Hyponoia < Formula
  desc "Code context engine for AI coding agents"
  homepage "https://github.com/patalbansishashank/hyponoia"
  version "0.3.0"
  license "MIT"

  # RETIRED-PLATFORM(macos): the on_macos block and its two darwin URLs were
  # removed — both assets stopped being published. A formula cannot print a
  # custom error, so `depends_on :linux` is how brew says it on macOS ("Linux is
  # required for this software.") before any download is attempted.
  # See docs/MAINTAINERS.md "Retired platforms".
  depends_on :linux

  # Checksums are placeholders until v0.3.0 publishes — see pkg/aur/PKGBUILD
  # for why they are zeroed rather than left at upstream's values.
  # The build publishes only the UI variant, so these are the only archives that exist.
  on_linux do
    on_arm do
      url "https://github.com/patalbansishashank/hyponoia/releases/download/v#{version}/hyponoia-ui-linux-arm64.tar.gz"
      sha256 "5fa1342d7ebfb00e32048fcf39bf21a7314a0a07af3a7e045a46ecea0303b720"
    end
    on_intel do
      url "https://github.com/patalbansishashank/hyponoia/releases/download/v#{version}/hyponoia-ui-linux-amd64.tar.gz"
      sha256 "baecf99e5ad46976d75a96ffd2a71a145e68b09710a2b32f983a4202afc73d60"
    end
  end

  def install
    bin.install "hyponoia"
    (share/"hyponoia").install "hyp-integrations.json"
    # Graph UI asset pack. Without it the UI has no assets to serve, so the
    # archive's sixth member cannot be dropped on the floor. The name is the
    # sha256 of the pack's own bytes, so it can only ever be matched by glob.
    # share/hyponoia is what the runtime resolves as
    # <bindir>/../share/hyponoia/<pack> (src/ui/asset_pack.c), which is the
    # FHS-shaped location this formula already installs into — bin/ is for the
    # executable, and Homebrew links it out of the Cellar either way.
    # Guarded like THIRD_PARTY_NOTICES.md below: archives older than this
    # formula predate the pack and must still install.
    ui_packs = Dir["hyp-ui-*.pack"]
    (share/"hyponoia").install ui_packs unless ui_packs.empty?
    # Third-party attribution bundle. Guarded: archives older than this
    # formula predate it, and the version it first appeared in was upstream's.
    doc.install "THIRD_PARTY_NOTICES.md" if File.exist?("THIRD_PARTY_NOTICES.md")
  end

  def caveats
    <<~EOS
      Run the following to configure your coding agents:
        hyponoia install

      To tap this formula directly:
        brew tap patalbansishashank/hyponoia https://github.com/patalbansishashank/hyponoia
        brew install hyponoia
    EOS
  end

  test do
    assert_match "hyponoia", shell_output("#{bin}/hyponoia --version")
    assert_path_exists share/"hyponoia/hyp-integrations.json"
  end
end
