/// <reference types="tree-sitter-cli/dsl" />

const PREC = {
  lambda: -1,
  or: 1,
  and: 2,
  compare: 3,
  coalesce: 4,
  range: 5,
  bitor: 6,
  bitxor: 7,
  bitand: 8,
  shift: 9,
  additive: 10,
  multiplicative: 11,
  cast: 12,
  unary: 13,
  postfix: 14,
};

const KEYWORDS = [
  'package', 'import', 'pub', 'internal', 'private', 'let', 'var', 'const', 'fn', 'suspend', 'struct', 'state',
  'enum', 'trait', 'impl', 'if', 'else', 'match', 'for', 'in', 'while', 'break', 'continue', 'return', 'wait', 'at',
  'scope', 'every', 'ref', 'as', 'is', 'true', 'false', 'null', 'self',
];

const ASSIGNMENT_OPERATORS = ['=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>='];

function commaSep(rule) {
  return optional(commaSep1(rule));
}

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)), optional(','));
}

module.exports = grammar({
  name: 'quartz',

  word: $ => $.identifier,

  externals: $ => [
    $._automatic_semicolon,
    $.string_content,
    $._multiline_string_content,
    $.block_comment,
    $._error_sentinel,
  ],

  extras: $ => [/\s/, $.line_comment, $.block_comment],

  supertypes: $ => [$._declaration, $._expression, $._type, $._pattern],

  inline: $ => [$._terminator, $._type_name],

  conflicts: $ => [
    [$._expression, $.lambda_parameter],
    [$.unit_expression, $.lambda_parameters],
  ],

  rules: {
    source_file: $ => repeat(choice($.package_declaration, $.import_declaration, $._declaration)),

    _terminator: $ => choice(';', $._automatic_semicolon),

    package_declaration: $ => seq('package', field('name', $.path), $._terminator),

    path: $ => prec.right(seq($.identifier, repeat(seq('.', $.identifier)))),

    import_declaration: $ => seq(
      'import',
      field('path', $.import_path),
      optional(seq('as', field('alias', $.identifier))),
      $._terminator,
    ),

    import_path: $ => seq(
      $.identifier,
      repeat(seq('.', choice($.identifier, $.import_wildcard, $.import_list))),
    ),

    import_wildcard: _ => '*',

    import_list: $ => seq('{', commaSep($.identifier), '}'),

    _declaration: $ => choice(
      $.function_declaration,
      $.struct_declaration,
      $.enum_declaration,
      $.trait_declaration,
      $.impl_declaration,
      $.const_declaration,
    ),

    attribute: $ => seq(
      '@',
      field('name', choice($.identifier, alias(choice(...KEYWORDS), $.identifier))),
      optional(field('arguments', $.attribute_arguments)),
    ),

    attribute_arguments: $ => seq('(', commaSep(choice($._expression, $.named_argument)), ')'),

    visibility_modifier: _ => choice('pub', 'internal', 'private'),

    function_declaration: $ => seq(
      repeat($.attribute),
      optional($.visibility_modifier),
      optional('suspend'),
      'fn',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameters)),
      field('parameters', $.parameters),
      optional(seq('->', field('return_type', $._type))),
      choice(field('body', $.block), $._terminator),
    ),

    parameters: $ => seq('(', commaSep(choice($.self_parameter, $.parameter)), ')'),

    self_parameter: _ => 'self',

    parameter: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $._type),
      optional(seq('=', field('default', $._expression))),
    ),

    type_parameters: $ => seq('<', commaSep($.type_parameter), '>'),

    type_parameter: $ => seq(
      field('name', alias($.identifier, $.type_identifier)),
      optional(seq(':', field('bounds', $.trait_bounds))),
    ),

    trait_bounds: $ => seq(
      alias($.identifier, $.type_identifier),
      repeat(seq('+', alias($.identifier, $.type_identifier))),
    ),

    struct_declaration: $ => seq(
      repeat($.attribute),
      optional($.visibility_modifier),
      choice('struct', 'state'),
      field('name', alias($.identifier, $.type_identifier)),
      optional(field('type_parameters', $.type_parameters)),
      choice(field('body', $.field_declaration_list), $._terminator),
    ),

    field_declaration_list: $ => seq(
      '{',
      repeat(seq($.field_declaration, choice(',', $._terminator))),
      '}',
    ),

    field_declaration: $ => seq(
      repeat($.attribute),
      field('name', $.identifier),
      ':',
      field('type', $._type),
      optional(seq('=', field('default', $._expression))),
    ),

    enum_declaration: $ => seq(
      repeat($.attribute),
      optional($.visibility_modifier),
      'enum',
      field('name', alias($.identifier, $.type_identifier)),
      optional(field('type_parameters', $.type_parameters)),
      field('body', $.enum_variant_list),
    ),

    enum_variant_list: $ => seq(
      '{',
      repeat(seq($.enum_variant, choice(',', $._automatic_semicolon))),
      '}',
    ),

    enum_variant: $ => seq(
      field('name', $.identifier),
      optional(field('payload', $.variant_payload)),
    ),

    variant_payload: $ => seq('(', commaSep($._type), ')'),

    trait_declaration: $ => seq(
      repeat($.attribute),
      optional($.visibility_modifier),
      'trait',
      field('name', alias($.identifier, $.type_identifier)),
      field('body', $.declaration_list),
    ),

    impl_declaration: $ => seq(
      repeat($.attribute),
      optional($.visibility_modifier),
      'impl',
      optional(field('type_parameters', $.type_parameters)),
      choice(
        field('type', $._type),
        seq(field('trait', $._type), 'for', field('type', $._type)),
      ),
      field('body', $.declaration_list),
    ),

    declaration_list: $ => seq('{', repeat($.function_declaration), '}'),

    const_declaration: $ => seq(
      repeat($.attribute),
      optional($.visibility_modifier),
      'const',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      '=',
      field('value', $._expression),
      $._terminator,
    ),

    block: $ => seq('{', repeat(choice($._statement, ';')), '}'),

    _statement: $ => choice(
      seq($._simple_statement, $._terminator),
      $._compound_statement,
    ),

    _simple_statement: $ => choice(
      $.let_declaration,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.assignment_statement,
      $.expression_statement,
    ),

    _compound_statement: $ => choice(
      $.for_statement,
      $.while_statement,
      $.scope_statement,
      $.every_statement,
      $.at_statement,
    ),

    let_declaration: $ => seq(
      choice('let', 'var', 'const', 'ref'),
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('value', $._expression))),
    ),

    return_statement: $ => prec.right(seq('return', optional($._expression))),

    break_statement: _ => 'break',

    continue_statement: _ => 'continue',

    assignment_statement: $ => seq(
      field('left', $._expression),
      field('operator', choice(...ASSIGNMENT_OPERATORS)),
      field('right', $._expression),
    ),

    expression_statement: $ => $._expression,

    for_statement: $ => seq(
      'for',
      optional(seq(field('index', $.identifier), ',')),
      field('item', $.identifier),
      'in',
      field('iterable', $._expression),
      field('body', $.block),
    ),

    while_statement: $ => seq('while', field('condition', $._expression), field('body', $.block)),

    scope_statement: $ => seq('scope', field('body', $.block)),

    every_statement: $ => seq('every', field('interval', $._expression), field('body', $.block)),

    at_statement: $ => seq('at', field('context', $._expression), field('body', $.block)),

    _expression: $ => choice(
      $.binary_expression,
      $.range_expression,
      $.unary_expression,
      $.wait_expression,
      $.cast_expression,
      $.is_expression,
      $.call_expression,
      $.index_expression,
      $.field_expression,
      $.try_expression,
      $.lambda_expression,
      $.parenthesized_expression,
      $.tuple_expression,
      $.unit_expression,
      $.list_expression,
      $.list_repeat_expression,
      $.if_expression,
      $.match_expression,
      $.block,
      $.generic_function,
      $.identifier,
      $.self,
      $._literal,
    ),

    _literal: $ => choice(
      $.integer_literal,
      $.float_literal,
      $.char_literal,
      $.string,
      $.boolean_literal,
      $.null_literal,
    ),

    binary_expression: $ => {
      const table = [
        [prec.left, PREC.or, '||'],
        [prec.left, PREC.and, '&&'],
        [prec.left, PREC.compare, choice('==', '!=', '<', '<=', '>', '>=')],
        [prec.right, PREC.coalesce, '??'],
        [prec.left, PREC.bitor, '|'],
        [prec.left, PREC.bitxor, '^'],
        [prec.left, PREC.bitand, '&'],
        [prec.left, PREC.shift, choice('<<', '>>')],
        [prec.left, PREC.additive, choice('+', '-')],
        [prec.left, PREC.multiplicative, choice('*', '/', '%')],
      ];
      return choice(...table.map(([assoc, level, operator]) => assoc(level, seq(
        field('left', $._expression),
        field('operator', operator),
        field('right', $._expression),
      ))));
    },

    range_expression: $ => prec.left(PREC.range, seq(
      field('start', $._expression),
      field('operator', choice('..', '..=')),
      field('end', $._expression),
    )),

    unary_expression: $ => prec(PREC.unary, seq(
      field('operator', choice('-', '!', '~')),
      field('operand', $._expression),
    )),

    wait_expression: $ => prec.right(PREC.range, seq('wait', field('value', $._expression))),

    cast_expression: $ => prec.left(PREC.cast, seq(
      field('value', $._expression),
      'as',
      field('type', $._type),
    )),

    is_expression: $ => prec.left(PREC.cast, seq(
      field('value', $._expression),
      'is',
      field('type', $._type),
    )),

    call_expression: $ => prec(PREC.postfix, seq(
      field('function', $._expression),
      field('arguments', $.arguments),
    )),

    arguments: $ => seq('(', commaSep(choice($._expression, $.named_argument)), ')'),

    named_argument: $ => seq(field('name', $.identifier), '=', field('value', $._expression)),

    index_expression: $ => prec(PREC.postfix, seq(
      field('value', $._expression),
      '[',
      field('index', $._expression),
      ']',
    )),

    field_expression: $ => prec(PREC.postfix, seq(
      field('value', $._expression),
      choice('.', alias('?.', $.safe_navigation)),
      field('field', choice($.identifier, alias($.integer_literal, $.tuple_index))),
    )),

    try_expression: $ => prec(PREC.postfix, seq($._expression, '?')),

    generic_function: $ => seq(
      field('function', $.identifier),
      '::',
      field('type_arguments', $.type_arguments),
    ),

    lambda_expression: $ => prec.right(PREC.lambda, seq(
      field('parameters', $.lambda_parameters),
      '=>',
      field('body', $._expression),
    )),

    lambda_parameters: $ => seq('(', commaSep($.lambda_parameter), ')'),

    lambda_parameter: $ => seq(
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
    ),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    tuple_expression: $ => seq(
      '(',
      $._expression,
      ',',
      optional(seq($._expression, repeat(seq(',', $._expression)), optional(','))),
      ')',
    ),

    unit_expression: _ => seq('(', ')'),

    list_expression: $ => seq('[', commaSep($._expression), ']'),

    list_repeat_expression: $ => seq(
      '[',
      field('value', $._expression),
      ';',
      field('count', $._expression),
      ']',
    ),

    if_expression: $ => prec.right(seq(
      'if',
      field('condition', choice($._expression, $.let_condition)),
      field('consequence', $.block),
      optional(seq('else', field('alternative', choice($.block, $.if_expression)))),
    )),

    let_condition: $ => seq('let', field('name', $.identifier), '=', field('value', $._expression)),

    match_expression: $ => seq('match', field('value', $._expression), field('body', $.match_block)),

    match_block: $ => seq(
      '{',
      repeat(seq($.match_arm, choice(',', $._terminator))),
      '}',
    ),

    match_arm: $ => seq(
      field('pattern', $._pattern),
      optional(seq('if', field('guard', $._expression))),
      '=>',
      field('body', choice(
        $._expression,
        $.let_declaration,
        $.return_statement,
        $.break_statement,
        $.continue_statement,
        $.assignment_statement,
        $._compound_statement,
      )),
    ),

    _pattern: $ => choice(
      $.wildcard_pattern,
      $.identifier,
      $.variant_pattern,
      $._literal_pattern,
      $.range_pattern,
    ),

    wildcard_pattern: _ => '_',

    variant_pattern: $ => choice(
      seq(field('name', $.identifier), field('arguments', $.pattern_arguments)),
      seq(field('name', $.scoped_identifier), optional(field('arguments', $.pattern_arguments))),
    ),

    scoped_identifier: $ => seq($.identifier, repeat1(seq('.', $.identifier))),

    pattern_arguments: $ => seq('(', commaSep($._pattern), ')'),

    _literal_pattern: $ => choice($._literal, $.negative_literal, $.self),

    negative_literal: $ => seq('-', choice($.integer_literal, $.float_literal)),

    range_pattern: $ => seq(
      field('start', $._literal_pattern),
      field('operator', choice('..', '..=')),
      field('end', $._literal_pattern),
    ),

    _type: $ => choice(
      alias($.identifier, $.type_identifier),
      $.scoped_type_identifier,
      $.generic_type,
      $.list_type,
      $.array_type,
      $.tuple_type,
      $.function_type,
      $.nullable_type,
      $.self_type,
    ),

    _type_name: $ => choice(alias($.identifier, $.type_identifier), $.scoped_type_identifier),

    scoped_type_identifier: $ => seq(
      field('path', alias($._type_path, $.path)),
      '.',
      field('name', alias($.identifier, $.type_identifier)),
    ),

    _type_path: $ => prec(1, choice($.identifier, seq($._type_path, '.', $.identifier))),

    self_type: _ => 'self',

    generic_type: $ => prec(1, seq(
      field('type', $._type_name),
      field('type_arguments', $.type_arguments),
    )),

    type_arguments: $ => seq('<', commaSep($._type), '>'),

    list_type: $ => seq('[', field('element', $._type), ']'),

    array_type: $ => seq('[', field('element', $._type), ';', field('length', $._expression), ']'),

    tuple_type: $ => seq('(', commaSep($._type), ')'),

    function_type: $ => prec.right(seq(
      'fn',
      field('parameters', alias($.tuple_type, $.parameter_types)),
      optional(seq('->', field('return_type', $._type))),
    )),

    nullable_type: $ => prec(PREC.postfix + 1, seq($._type, '?')),

    integer_literal: _ => token(choice(
      /[0-9][0-9_]*/,
      /0[xX][0-9a-fA-F_]*/,
      /0[bB][01_]*/,
      /0[oO][0-7_]*/,
    )),

    float_literal: _ => token(choice(
      /[0-9][0-9_]*\.[0-9][0-9_]*([eE][+-]?[0-9][0-9_]*)?/,
      /[0-9][0-9_]*[eE][+-]?[0-9][0-9_]*/,
    )),

    boolean_literal: _ => choice('true', 'false'),

    null_literal: _ => 'null',

    self: _ => 'self',

    char_literal: _ => token(seq(
      '\'',
      choice(
        /[^'\\\n]/,
        /\\[ntr0\\"'{}]/,
        /\\u\{[0-9a-fA-F]{1,7}\}/,
      ),
      '\'',
    )),

    string: $ => choice(
      seq(
        '"',
        repeat(choice($.string_content, $.escape_sequence, $.interpolation)),
        token.immediate('"'),
      ),
      seq(
        '"""',
        repeat(choice(
          alias($._multiline_string_content, $.string_content),
          $.escape_sequence,
          $.interpolation,
        )),
        token.immediate('"""'),
      ),
    ),

    escape_sequence: _ => token.immediate(choice(
      /\\[ntr0\\"'{}]/,
      /\\u\{[0-9a-fA-F]{1,7}\}/,
    )),

    interpolation: $ => seq(token.immediate('{'), $._expression, '}'),

    identifier: _ => /[A-Za-z_][A-Za-z0-9_]*/,

    line_comment: _ => token(seq('//', /[^\n]*/)),
  },
});
