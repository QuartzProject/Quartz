; Scopes

[
  (source_file)
  (function_declaration)
  (impl_declaration)
  (trait_declaration)
  (block)
  (lambda_expression)
  (for_statement)
  (if_expression)
  (match_arm)
] @local.scope

; Definitions

(function_declaration
  name: (identifier) @local.definition.function)

(parameter
  name: (identifier) @local.definition.parameter)

(lambda_parameter
  name: (identifier) @local.definition.parameter)

(let_declaration
  name: (identifier) @local.definition.var)

(let_condition
  name: (identifier) @local.definition.var)

(for_statement
  index: (identifier) @local.definition.var)

(for_statement
  item: (identifier) @local.definition.var)

(match_arm
  pattern: (identifier) @local.definition.var)

(pattern_arguments
  (identifier) @local.definition.var)

(tuple_pattern
  (identifier) @local.definition.var)

(const_declaration
  name: (identifier) @local.definition.constant)

(struct_declaration
  name: (type_identifier) @local.definition.type)

(enum_declaration
  name: (type_identifier) @local.definition.type)

(trait_declaration
  name: (type_identifier) @local.definition.type)

(type_parameter
  name: (type_identifier) @local.definition.type)

(field_declaration
  name: (identifier) @local.definition.field)

(enum_variant
  name: (identifier) @local.definition.enum)

(import_list
  (identifier) @local.definition.import)

(import_declaration
  alias: (identifier) @local.definition.import)

; References

(identifier) @local.reference

(type_identifier) @local.reference
