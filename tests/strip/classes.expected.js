         class Base                                     { x; y;
  ;                      
  ;                               
  ;                   
                   id         ;
  static count         = 0;
  constructor(       x        ,                    y = 2) { this.x = x; this.y = y;}
  ;                       
  ;                         
  ;                         
  overload(a         )       {}
  get size()         { return 1; }
  *[Symbol.iterator]()              {}
}
class Child extends Base         { z;
  constructor(        z        ) {
    super(1); this.z = z;
    this.z;
  }
           area()         { return 0; }
}
