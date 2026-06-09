#include "world/ComponentRegistry.h"
#include "world/ComponentSet.h"
#include "reflection/Reflection.h"
#include "test_framework.h"

using namespace iron;

namespace {
struct Widget { int n = 0; float f = 0.0f; };
}  // namespace

int main() {
    Reflection r;
    r.registerType<Widget>("Widget")
        .field("n", &Widget::n)
        .field("f", &Widget::f);

    ComponentRegistry reg;
    reg.registerComponent<Widget>("Widget", r);

    // lookup by typeId and by name resolve to the same entry
    const ComponentRegistry::Entry* byId   = reg.byTypeId(componentTypeId<Widget>());
    const ComponentRegistry::Entry* byName = reg.byName("Widget");
    CHECK(byId != nullptr);
    CHECK(byName != nullptr);
    CHECK(byId == byName);
    CHECK(byId->name == "Widget");
    CHECK(byId->fields.size() == 2u);              // captured from Reflection

    // order() lists the registered type
    CHECK(reg.order().size() == 1u);
    CHECK(reg.order()[0] == componentTypeId<Widget>());

    // factory produces a box of the right type that ComponentSet accepts
    auto box = byId->factory();
    CHECK(box != nullptr);
    CHECK(box->typeId() == componentTypeId<Widget>());
    static_cast<Widget*>(box->data())->n = 5;
    ComponentSet cs;
    cs.addBox(std::move(box));
    CHECK(cs.get<Widget>() != nullptr);
    CHECK(cs.get<Widget>()->n == 5);

    // unknown lookups → null
    CHECK(reg.byName("Nope") == nullptr);
    CHECK(reg.byTypeId(250) == nullptr);

    return iron_test_result();
}
