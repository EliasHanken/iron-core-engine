#include "world/ComponentSet.h"
#include "test_framework.h"
#include <string>

using namespace iron;

namespace {
struct A { int x = 0; };
struct B { float y = 0.0f; std::string s; };
}  // namespace

int main() {
    // add / get / has / remove
    {
        ComponentSet cs;
        CHECK(!cs.has<A>());
        A* a = cs.add<A>(A{7});
        CHECK(a != nullptr);
        CHECK(cs.has<A>());
        CHECK(cs.get<A>() != nullptr);
        CHECK(cs.get<A>()->x == 7);
        CHECK(cs.get<B>() == nullptr);          // absent type -> null

        // add-or-replace: adding A again replaces it
        cs.add<A>(A{9});
        CHECK(cs.get<A>()->x == 9);

        cs.add<B>(B{1.5f, "hi"});
        CHECK(cs.has<B>());
        CHECK(cs.get<B>()->s == "hi");

        cs.remove<A>();
        CHECK(!cs.has<A>());
        CHECK(cs.has<B>());                     // unrelated type untouched
    }

    // deep copy independence (matters for Play/Stop scene snapshots)
    {
        ComponentSet original;
        original.add<A>(A{1});
        ComponentSet copy = original;           // copy ctor -> clone each box
        copy.get<A>()->x = 42;
        CHECK(original.get<A>()->x == 1);        // original unaffected
        CHECK(copy.get<A>()->x == 42);

        ComponentSet assigned;
        assigned = original;                     // copy assignment
        assigned.get<A>()->x = 99;
        CHECK(original.get<A>()->x == 1);
    }

    // generic iteration: all() exposes typeId + data()
    {
        ComponentSet cs;
        cs.add<A>(A{3});
        cs.add<B>(B{2.0f, "z"});
        int count = 0;
        bool sawA = false, sawB = false;
        for (const auto& box : cs.all()) {
            ++count;
            if (box->typeId() == componentTypeId<A>()) {
                sawA = true;
                CHECK(static_cast<const A*>(box->data())->x == 3);
            }
            if (box->typeId() == componentTypeId<B>()) sawB = true;
        }
        CHECK(count == 2);
        CHECK(sawA && sawB);
        CHECK(cs.hasTypeId(componentTypeId<A>()));
        cs.removeTypeId(componentTypeId<A>());
        CHECK(!cs.has<A>());
    }

    return iron_test_result();
}
