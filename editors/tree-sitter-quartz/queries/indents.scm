[
  (block)
  (field_declaration_list)
  (enum_variant_list)
  (declaration_list)
  (match_block)
  (import_list)
  (parameters)
  (arguments)
  (attribute_arguments)
  (lambda_parameters)
  (pattern_arguments)
  (variant_payload)
  (list_expression)
  (list_repeat_expression)
  (tuple_expression)
  (parenthesized_expression)
  (type_arguments)
  (type_parameters)
] @indent.begin

[
  "}"
  "]"
  ")"
] @indent.branch @indent.end

(type_arguments
  ">" @indent.branch @indent.end)

(type_parameters
  ">" @indent.branch @indent.end)

(if_expression
  "else" @indent.branch)

(interpolation) @indent.ignore

[
  (string)
  (block_comment)
] @indent.auto

(line_comment) @indent.ignore
