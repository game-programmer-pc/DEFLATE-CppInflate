# DEFLATE-CppInflate
A platform agnostic deflate decompressor (inflate). No need for make or any other external tools. For any platform with a C++20 compliant compiler, the header and source file can simply be dropped into any project.
*WARNING* There are two edge cases, one being if the dynamic block causes huffman code ambiguity. If this is the case, the results are unknown. The other is when a malicious datastream is constructed to inflate by an excessive amount. In this case, you will eventually run out of memory.
