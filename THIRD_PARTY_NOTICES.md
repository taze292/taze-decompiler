# Luau

This project uses the bytecode definitions and studies the serializer and VM implementation from [luau-lang/luau](https://github.com/luau-lang/luau). The Luau source checkout is a separate, untracked build dependency. The tests and fixture compiler link Luau libraries; the decompiler includes Luau's bytecode headers.

Luau is distributed under the MIT license. Its license notice is retained in `docs/Luau-LICENSE.txt`. Taze's parser, dataflow analysis, source reconstruction, CLI, and test harness are implemented in this repository.
