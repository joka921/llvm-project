//
// Created by kalmbacj on 3/11/25.
//

#ifndef JOKA_EXAMPLES_H
#define JOKA_EXAMPLES_H
namespace outer {
namespace blix {
struct Blix {
 bool operator<=>(const Blix& otherRhs) const = default;
 };
 }

 struct Bla {
template <typename T, int NumColumns = 0>
class Templated {
  bool operator==(const Templated& ) const {
    return true;
  }
};
};
struct C : public blix::Blix {
    int x;
    bool operator==(const C& otherRhs) const {
    if (!(static_cast<const outer::blix::Blix&>(*this) == static_cast<const outer::blix::Blix&>(otherRhs))) return false;
    if (!(x == otherRhs.x)) return false;
    if (!(f == otherRhs.f)) return false;
    if (!(d == otherRhs.d)) return false;
    return true;
  };

    bool f;
    static int s;
    unsigned d = 3;
};
}

#endif //JOKA_EXAMPLES_H
