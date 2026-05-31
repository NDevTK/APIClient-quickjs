var o = __opaque();
// Each should STAY opaque (so the host edge shapes it {name}), not stringify to "[object Object]"
print("concat_right: " + __isOpaque("/api/" + o));
print("concat_left:  " + __isOpaque(o + "/api"));
print("template:     " + __isOpaque(`/api/${o}/x`));
print("String():     " + __isOpaque(String(o)));
print("encodeURIComponent: " + __isOpaque(encodeURIComponent(o)));
print("encodeURI:    " + __isOpaque(encodeURI("/api/" + o)));
print("arr.join:     " + __isOpaque(["/api", o, "x"].join("/")));
print("concat_method:" + __isOpaque("/api/".concat(o)));
