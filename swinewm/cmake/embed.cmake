# Turns a text file into a C header defining a NUL-terminated char array,
# so built-in defaults are the same files that get installed.
# Usage: cmake -DINPUT=file -DOUTPUT=header.h -DNAME=symbol -P embed.cmake
get_filename_component(input_name "${INPUT}" NAME)
file(READ "${INPUT}" content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," content "${content}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)"
  "\\1\n" content "${content}")
file(WRITE "${OUTPUT}"
  "/* Generated from ${input_name} by cmake/embed.cmake. Do not edit. */\n"
  "static const char ${NAME}[] = {\n${content}0x00};\n")
