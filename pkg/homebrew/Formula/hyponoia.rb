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
  on_linux do
    on_arm do
      url "https://github.com/patalbansishashank/hyponoia/releases/download/v#{version}/hyponoia-linux-arm64.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
    on_intel do
      url "https://github.com/patalbansishashank/hyponoia/releases/download/v#{version}/hyponoia-linux-amd64.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  def install
    bin.install "hyponoia"
    (share/"hyponoia").install "hyp-integrations.json"
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
