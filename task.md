你需要完成以下任务：

1. 参考template.bs和C++标准草案，将proposal.md重新写为一个正式的C++标准提案proposal.bs
2. 根据proposal.md和C++标准，为llvm实现该特性做出计划，编写为plan.md（当前文件夹就是llvm文件夹），至少包括对SemaCoroutine.cpp的更改，GCCoroutine.cpp的更改，coroutine_handle的更改，intrinsic的更改，是否需要添加新的警告，设计新的ABI扩展点（不能破坏现有ABI），新的测试文件
3. 实施第二步的计划。
4. 根据提案的核心要义修改generator.hpp为generator_improved.hpp，使得嵌套generator使用提案所述功能避免异常重复抛出
5. 使用generator.hpp和generator_improved.hpp编写性能测试对比，不需要使用第三方库，使用C++标准库简单测量时间即可
6. 根据结论完善提案

你需要将每个任务分解为小任务，持续迭代，每次更改代码，都需要记录实现状态。注意不需要记录失败的东西。每个小任务执行完成后，都需要重新分析是否可以改进，并删除冗余

提案所需材料在proposal_assert文件夹中，当前根目录是llvm源码，C++标准草案在draft文件夹中。