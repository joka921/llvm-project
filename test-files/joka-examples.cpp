struct C {
  int x;
  bool operator==(const C&) const = default;
  bool f;
  static int s;
  unsigned d = 3;
  int arr[3];
};

struct D {
  void f() {}
  bool operator==(const D&) const { return true;}
  bool b;
};

struct E {
  int ex;
  bool operator==(const E&) const = default;
  bool ef;
  static int s;
  unsigned ed = 3;
  int earr[3];
};

void f() {
  for (int i = 0; i < 54; ++i) {
  }
}
