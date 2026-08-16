class Hyponoia < Formula
  desc "Code context engine for AI coding agents"
  homepage "https://github.com/patalbansishashank/hyponoia"
  version "0.3.1"
  license "MIT"

  # RETIRED-PLATFORM(macos): the on_macos block and its two darwin URLs were
  # removed — both assets stopped being published. A formula cannot print a
  # custom error, so `depends_on :linux` is how brew says it on macOS ("Linux is
  # required for this software.") before any download is attempted.
  # See docs/MAINTAINERS.md "Retired platforms".
  depends_on :linux

  # The `-portable` (fully static) archives, not the ordinary ones. Homebrew on
  # Linux exists largely to serve distributions older than the host toolchain,
  # and the ordinary linux archive links glibc 2.38+ / GLIBCXX_3.4.32: installed
  # into the homebrew/brew container (Ubuntu 22.04, glibc 2.35) it resolves,
  # verifies and links, and then `hyponoia --version` dies with
  # "version `GLIBC_2.38' not found". A formula that installs a binary the
  # platform cannot execute has not installed anything. The static archive has
  # the same six members and the same layout, so nothing below changes.
  # Keep in sync with pkg/npm/install.js and pkg/pypi/src/hyponoia/_cli.py,
  # which pick `-portable` on Linux for exactly this reason.
  #
  # Checksums are v0.3.1's own, copied from the release's checksums.txt and
  # verified independently by re-hashing the downloaded archives.
  # The build publishes only the UI variant, so these are the only archives that exist.
  on_linux do
    on_arm do
      url "https://github.com/patalbansishashank/hyponoia/releases/download/v#{version}/hyponoia-ui-linux-arm64-portable.tar.gz"
      sha256 "43f4be9c9675a41f666902921ea1c7b4c4d3f9b9b1ff0352272595fbf90ceee4"
    end
    on_intel do
      url "https://github.com/patalbansishashank/hyponoia/releases/download/v#{version}/hyponoia-ui-linux-amd64-portable.tar.gz"
      sha256 "045e2e0810ad03fceca716eeee916b7257f325f83ddcda17ae5b53c50a9a76b8"
    end
  end

  def install
    bin.install "hyponoia"
    # `pkgshare` is share/"hyponoia" — spelled the way `brew audit --strict`
    # demands, which is the spelling homebrew-core and every tap reviewer sees.
    pkgshare.install "hyp-integrations.json"
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
    pkgshare.install ui_packs unless ui_packs.empty?
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
    assert_path_exists pkgshare/"hyp-integrations.json"
  end
end
