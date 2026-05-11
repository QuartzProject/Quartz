; Identifiers

(identifier) @variable

((identifier) @constant
  (#match? @constant "^[A-Z][A-Z0-9_]*[A-Z0-9]$"))

(self) @variable.builtin

(self_parameter) @variable.builtin

(parameter
  name: (identifier) @variable.parameter)

(lambda_parameter
  name: (identifier) @variable.parameter)

(named_argument
  name: (identifier) @variable.parameter)

(field_declaration
  name: (identifier) @variable.member)

(field_expression
  field: (identifier) @variable.member)

(field_expression
  field: (tuple_index) @variable.member)

((field_expression
  value: (identifier) @type)
  (#match? @type "^[A-Z]"))

(const_declaration
  name: (identifier) @constant)

(wildcard_pattern) @character.special

; Types

(type_identifier) @type

((type_identifier) @type.builtin
  (#any-of? @type.builtin
    "int" "float" "byte" "bool" "char" "str" "void" "Bytes" "Self" "i8" "i16" "i32" "i64" "u8" "u16" "u32" "u64"
    "f32" "f64"))

(self_type) @type.builtin

(enum_variant
  name: (identifier) @constant)

(variant_pattern
  name: (identifier) @constant)

(variant_pattern
  name: (scoped_identifier
    (identifier) @constant .))

; Modules

(package_declaration
  name: (path
    (identifier) @module))

(import_path
  (identifier) @module)

(import_declaration
  alias: (identifier) @module)

(scoped_type_identifier
  path: (path
    (identifier) @module))

; Functions

(function_declaration
  name: (identifier) @function)

(declaration_list
  (function_declaration
    name: (identifier) @function.method))

(call_expression
  function: (identifier) @function.call)

(call_expression
  function: (field_expression
    field: (identifier) @function.method.call))

(generic_function
  function: (identifier) @function.call)

((call_expression
  function: (identifier) @constructor)
  (#match? @constructor "^[A-Z]"))

; Attributes

(attribute
  "@" @attribute
  name: (identifier) @attribute)

; Literals

(integer_literal) @number

(float_literal) @number.float

(boolean_literal) @boolean

(null_literal) @constant.builtin

(char_literal) @character

(string
  [
    "\""
    "\"\"\""
  ] @string)

(string_content) @string

(escape_sequence) @string.escape

(interpolation
  [
    "{"
    "}"
  ] @punctuation.special)

(interpolation
  ":" @punctuation.special)

(format_spec) @string.special

; Comments

(line_comment) @comment @spell

(block_comment) @comment @spell

((line_comment) @comment.documentation
  (#match? @comment.documentation "^///"))

; Keywords

[
  "let"
  "var"
  "const"
  "ref"
  "impl"
  "scope"
] @keyword

[
  "struct"
  "state"
  "enum"
  "trait"
] @keyword.type

"fn" @keyword.function

[
  "suspend"
  "wait"
  "at"
] @keyword.coroutine

(visibility_modifier) @keyword.modifier

"return" @keyword.return

[
  "if"
  "else"
  "match"
] @keyword.conditional

[
  "for"
  "in"
  "while"
  "every"
  "break"
  "continue"
] @keyword.repeat

[
  "package"
  "import"
] @keyword.import

[
  "as"
  "is"
] @keyword.operator

; Operators and punctuation

[
  "+"
  "-"
  "*"
  "/"
  "%"
  "&"
  "|"
  "^"
  "~"
  "!"
  "="
  "=="
  "!="
  "<"
  "<="
  ">"
  ">="
  "<<"
  ">>"
  "&&"
  "||"
  "??"
  ".."
  "..="
  "+="
  "-="
  "*="
  "/="
  "%="
  "&="
  "|="
  "^="
  "<<="
  ">>="
] @operator

(safe_navigation) @operator

(try_expression
  "?" @operator)

(nullable_type
  "?" @operator)

(import_wildcard) @character.special

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

(type_arguments
  [
    "<"
    ">"
  ] @punctuation.bracket)

(type_parameters
  [
    "<"
    ">"
  ] @punctuation.bracket)

[
  ","
  "."
  ":"
  "::"
  ";"
  "->"
  "=>"
] @punctuation.delimiter
