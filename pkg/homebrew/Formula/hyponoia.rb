class Hyponoia < Formula
  desc "Code context engine for AI coding agents"
  homepage "https://github.com/patalbansishashank/hyponoia"
  version "0.8.1"
  license "MIT"

  # RETIRED-PLATFORM(macos): the on_macos block and its two darwin URLs were
  # removed — both assets stopped being published. A formula cannot print a
  # custom error, so `depends_on :linux` is how brew says it on macOS ("Linux is
  # required for this software.") before any download is attempted.
  # See docs/MAINTAINERS.md "Retired platforms".
  depends_on :linux

  on_linux do
    on_arm do
      url "https://github.com/patalbansishashank/hyponoia/releases/download/v#{version}/hyponoia-linux-arm64.tar.gz"
      sha256 "d2f842d1365da5c35d9c5796f57a821c9745267350994346735e1e6e04d46091"
    end
    on_intel do
      url "https://github.com/patalbansishashank/hyponoia/releases/download/v#{version}/hyponoia-linux-amd64.tar.gz"
      sha256 "dbd3b92ea870ef240b63059f26bda15015f76ef9978931bebc3a0f9d09470973"
    end
  end

  def install
    bin.install "hyponoia"
    (share/"hyponoia").install "hyp-integrations.json"
    # Third-party attribution bundle (present in archives since v0.8.1)
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
