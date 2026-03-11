# Contributing

## Code Formatting

This project uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html)
with the configuration in `.clang-format` (Google style, 4-space indent, 132
column limit).

### Setup (one-time)

After cloning, install the git hooks:

```bash
bash scripts/install-hooks.sh
```

This enables a pre-commit hook that checks formatting on staged C/C++ files.
Commits with unformatted code will be rejected with a message showing which
files need formatting.

### Requirements

- **clang-format** must be on your PATH.
  - **Windows**: Included with Visual Studio (Developer Command Prompt) or
    install [LLVM](https://releases.llvm.org/).
  - **Linux**: `sudo apt install clang-format`
  - **macOS**: `brew install clang-format`

### Usage

Format a file:

```bash
clang-format -i path/to/file.cpp
```

Format all staged files before committing:

```bash
git diff --cached --name-only --diff-filter=ACM -- '*.c' '*.cpp' '*.h' '*.hpp' \
  | xargs clang-format -i
```

Check formatting without modifying (dry run):

```bash
clang-format --dry-run --Werror path/to/file.cpp
```

### Skipping the Hook

If you need to commit unformatted code temporarily:

```bash
git commit --no-verify
```

### Editor Integration

- **VS Code**: Install the "C/C++" extension, set
  `"editor.formatOnSave": true` — it picks up `.clang-format` automatically.
- **Visual Studio**: Tools → Options → Text Editor → C/C++ → Formatting →
  "Enable ClangFormat support".
- **vim**: `:ClangFormat` with the vim-clang-format plugin.
