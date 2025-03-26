#include "./joka-examples.h"
struct D {
  void f() {}
  bool operator==(const D&) const { return true;}
  bool b;
};

struct E {
  int ex;
  bool operator==(const E& otherRhs) const {
    if (ex != otherRhs.ex) return false;
    if (ef != otherRhs.ef) return false;
    if (ed != otherRhs.ed) return false;
    if (earr != otherRhs.earr) return false;
    return true;
  };
  bool ef;
  static int s;
  unsigned ed = 3;
  int earr[3];
};

void f() {
  for (int i = 0; i < 54; ++i) {
  }
}
