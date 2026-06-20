#include "sema/SemaImpl.hpp"

#include <format>

namespace quartz::sema {

TypeId Checker::checkWith(WithExpr* e, TypeId expected) {
    TypeId t = checkValue(e->value, expected);
    std::vector<std::uint32_t> index(e->fields.size(), ~0U);
    if (t != ir::kError && types_->kind(t) != ir::TypeKind::Struct) {
        diags_.error(e->value->span, std::format("'with' needs a struct value, found '{}'", typeName(t)));
        t = ir::kError;
    }
    for (std::size_t i = 0; i < e->fields.size(); ++i) {
        Arg& a = e->fields[i];
        TypeId ft = ir::kError;
        if (t != ir::kError) {
            const std::vector<ir::Field>& fields = types_->info(t).fields;
            for (std::uint32_t f = 0; f < fields.size(); ++f) {
                if (fields[f].name == str(a.name)) {
                    index[i] = f;
                    ft = fields[f].type;
                }
            }
            if (index[i] == ~0U) {
                diags_.error(a.value->span, std::format("'{}' has no field '{}'", typeName(t), str(a.name)));
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (e->fields[j].name == a.name) {
                    diags_.error(a.value->span, std::format("field '{}' is set twice", str(a.name)));
                }
            }
        }
        TypeId vt = checkValue(a.value, ft);
        if (ft != ir::kError) {
            expectAssignable(vt, ft, a.value->span);
        }
    }
    e->fieldIndex = arena_.copy(index);
    return t;
}

} // namespace quartz::sema
