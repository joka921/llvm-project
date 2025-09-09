//
// Created by kalmbacj on 3/11/25.
//

#ifndef JOKA_EXAMPLES_H
#define JOKA_EXAMPLES_H

#include <compare>
namespace outer {
namespace blix {
struct Blix {
 bool operator==(const Blix& ) const {
    return true;
  };
 };

struct Blax {
  bool operator==(const Blax& otherRhs) const {
    if (!(blubb ==  otherRhs.blubb)) { return false;}
    return true;
  }bool operator<(const Blax& otherRhs) const {
    if (!(blubb ==  otherRhs.blubb)) { return blubb < otherRhs.blubb;}
    return false;
  }bool operator<=(const Blax& otherRhs) const {
    if (!(blubb ==  otherRhs.blubb)) { return blubb <= otherRhs.blubb;}
    return true;
  }bool operator>(const Blax& otherRhs) const {
    if (!(blubb ==  otherRhs.blubb)) { return blubb > otherRhs.blubb;}
    return false;
  }bool operator>=(const Blax& otherRhs) const {
    if (!(blubb ==  otherRhs.blubb)) { return blubb >= otherRhs.blubb;}
    return true;
  }bool operator!=(const Blax& otherRhs) const {
    if (!(blubb ==  otherRhs.blubb)) { return true;}
    return false;
  };
  bool blubb;
};
}

 struct Bla {
template <typename T, int NumColumns = 0>
class Templated {
  bool operator==(const Templated& ) const {
    return true;
  };
};
};
struct C : public blix::Blix {
    int x;
    bool operator==(const C& otherRhs) const {
    if (!(static_cast<const outer::blix::Blix&>(*this) ==  static_cast<const outer::blix::Blix&>(otherRhs))) { return false;}
    if (!(x ==  otherRhs.x)) { return false;}
    if (!(f ==  otherRhs.f)) { return false;}
    if (!(d ==  otherRhs.d)) { return false;}
    return true;
  };

    bool f;
    static int s;
    unsigned d = 3;
};
}

#endif //JOKA_EXAMPLES_H
