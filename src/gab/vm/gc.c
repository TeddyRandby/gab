#include "../compiler/engine.h"
#include <stdio.h>

void *gab_reallocate(gab_engine *self, void *loc, u64 old_count,
                     u64 new_count) {
  if (new_count == 0) {
    free(loc);
    return NULL;
  }

#if GAB_DEBUG_GC
  if (new_count > old_count) {
    gab_engine_collect(self);
  };
#endif

  void *new_ptr = realloc(loc, new_count);

  if (!new_ptr) {

    gab_engine_collect(self);
    new_ptr = realloc(loc, new_count);

    if (!new_ptr) {
      exit(1);
    }
  }

  return new_ptr;
}

// Forward declare
void gab_engine_collect_cycles(gab_engine *gc);

static inline void queue_destroy(gab_engine *self, gab_obj *obj) {
  d_u64_insert(&self->gc.queue, GAB_VAL_OBJ(obj), GAB_VAL_NULL());
}

static inline void trigger_destroy(gab_engine *self) {
  for (i32 i = 0; i < self->gc.queue.cap; i++) {
    if (d_u64_istatus(&self->gc.queue, i) == D_FULL) {
      gab_value key = d_u64_ikey(&self->gc.queue, i);
      gab_obj_destroy(GAB_VAL_TO_OBJ(key), self);
      d_u64_iremove(&self->gc.queue, i);
    }
  }
}

static inline void push_root(gab_engine *self, gab_obj *obj) {
  if (self->gc.root_count >= INC_DEC_MAX) {
    gab_engine_collect(self);
  }

  d_u64_insert(&self->gc.roots, GAB_VAL_OBJ(obj), GAB_VAL_NULL());

#if GAB_DEBUG_GC
  gab_engine_collect_cycles(self);
#endif
}

static inline void obj_possible_root(gab_engine *self, gab_obj *obj) {
  if (!GAB_OBJ_IS_PURPLE(obj)) {
    GAB_OBJ_PURPLE(obj);
    if (!GAB_OBJ_IS_BUFFERED(obj)) {
      GAB_OBJ_BUFFERED(obj);
      push_root(self, obj);
    }
  }
}

typedef void (*child_and_gc_iter)(gab_engine *gc, gab_obj *obj);
static inline void for_child_and_gc_do(gab_engine *self, gab_obj *obj,
                                       child_and_gc_iter fnc) {
  gab_obj *child;
  switch (obj->kind) {
  case (OBJECT_CLOSURE): {
    gab_obj_closure *closure = (gab_obj_closure *)obj;
    for (u8 i = 0; i < closure->func->nupvalues; i++) {
      child = (gab_obj *)closure->upvalues[i];
      fnc(self, child);
    }
    return;
  }
  case (OBJECT_UPVALUE): {
    gab_obj_upvalue *upvalue = (gab_obj_upvalue *)obj;
    if (GAB_VAL_IS_OBJ(*upvalue->data)) {
      fnc(self, GAB_VAL_TO_OBJ(*upvalue->data));
    };
    return;
  }
  case OBJECT_SHAPE: {
    gab_obj_shape *shape = (gab_obj_shape *)obj;
    for (u64 i = 0; i < shape->properties.len; i++) {
      if (GAB_VAL_IS_OBJ(shape->keys[i])) {
        fnc(self, GAB_VAL_TO_OBJ(shape->keys[i]));
      }
    }
    return;
  }
  case (OBJECT_OBJECT): {
    gab_obj_object *object = (gab_obj_object *)obj;
    if (object->is_dynamic) {
      for (u64 i = 0; i < object->dynamic_values.len; i++) {
        if (GAB_VAL_IS_OBJ(object->dynamic_values.data[i])) {
          fnc(self, GAB_VAL_TO_OBJ(object->dynamic_values.data[i]));
        }
      }
    } else {
      for (u64 i = 0; i < object->static_size; i++) {
        if (GAB_VAL_IS_OBJ(object->static_values[i])) {
          fnc(self, GAB_VAL_TO_OBJ(object->static_values[i]));
        }
      }
    }
    return;
  }
  default:
    return;
  }
}

typedef void (*child_iter)(gab_obj *obj);
static inline void for_child_do(gab_obj *obj, child_iter fnc) {
  gab_obj *child;
  switch (obj->kind) {
  case (OBJECT_CLOSURE): {
    gab_obj_closure *closure = (gab_obj_closure *)obj;
    for (u8 i = 0; i < closure->func->nupvalues; i++) {
      child = (gab_obj *)closure->upvalues[i];
      fnc(child);
    }
    return;
  }
  case (OBJECT_UPVALUE): {
    gab_obj_upvalue *upvalue = (gab_obj_upvalue *)obj;
    if (GAB_VAL_IS_OBJ(*upvalue->data)) {
      fnc(GAB_VAL_TO_OBJ(*upvalue->data));
    };
    return;
  }
  case OBJECT_SHAPE: {
    gab_obj_shape *shape = (gab_obj_shape *)obj;
    for (u64 i = 0; i < shape->properties.len; i++) {
      if (GAB_VAL_IS_OBJ(shape->keys[i])) {
        fnc(GAB_VAL_TO_OBJ(shape->keys[i]));
      }
    }
    return;
  }
  case (OBJECT_OBJECT): {
    gab_obj_object *object = (gab_obj_object *)obj;

    if (object->is_dynamic) {
      for (u64 i = 0; i < object->dynamic_values.len; i++) {
        if (GAB_VAL_IS_OBJ(object->dynamic_values.data[i])) {
          fnc(GAB_VAL_TO_OBJ(object->dynamic_values.data[i]));
        }
      }
    } else {
      for (u64 i = 0; i < object->static_size; i++) {
        if (GAB_VAL_IS_OBJ(object->static_values[i])) {
          fnc(GAB_VAL_TO_OBJ(object->static_values[i]));
        }
      }
    }

    return;
  }
  default:
    return;
  }
}

// Forward declarations - these functions recurse into each other.
static inline boolean dec_obj_ref(gab_engine *self, gab_obj *obj);
static inline boolean dec_if_obj_ref(gab_engine *self, gab_value val);
static inline void dec_child_refs(gab_engine *self, gab_obj *obj) {
  switch (obj->kind) {
  case OBJECT_CLOSURE: {
    gab_obj_closure *closure = (gab_obj_closure *)obj;
    for (u8 i = 0; i < closure->func->nupvalues; i++) {
      dec_if_obj_ref(self, closure->upvalues[i]);
    }
    return;
  }
  case OBJECT_UPVALUE: {
    gab_obj_upvalue *upvalue = (gab_obj_upvalue *)obj;
    dec_if_obj_ref(self, *upvalue->data);
    return;
  }
  case OBJECT_SHAPE: {
    gab_obj_shape *shape = (gab_obj_shape *)obj;
    for (u64 i = 0; i < shape->properties.len; i++) {
      dec_if_obj_ref(self, shape->keys[i]);
    }
    return;
  }
  case OBJECT_OBJECT: {
    gab_obj_object *object = (gab_obj_object *)obj;
    if (object->is_dynamic) {
      for (u64 i = 0; i < object->dynamic_values.len; i++) {
        if (GAB_VAL_IS_OBJ(object->dynamic_values.data[i])) {
          dec_if_obj_ref(self, object->dynamic_values.data[i]);
        }
      }
    } else {
      for (u64 i = 0; i < object->static_size; i++) {
        if (GAB_VAL_IS_OBJ(object->static_values[i])) {
          dec_if_obj_ref(self, object->static_values[i]);
        }
      }
    }
    return;
  }
  default:
    return;
  }
}

static inline boolean dec_obj_ref(gab_engine *self, gab_obj *obj) {
  if (--obj->references == 0) {
    dec_child_refs(self, obj);

    GAB_OBJ_BLACK(obj);

    if (!GAB_OBJ_IS_BUFFERED(obj)) {
      queue_destroy(self, obj);
      return true;
    }

  } else {
    obj_possible_root(self, obj);
  }

  return false;
}

static inline boolean dec_if_obj_ref(gab_engine *self, gab_value val) {
  if (GAB_VAL_IS_OBJ(val)) {
    gab_obj *obj = GAB_VAL_TO_OBJ(val);
    return dec_obj_ref(self, obj);
  }
  return false;
}

static inline void inc_obj_ref(gab_obj *obj) {
  obj->references++;
  // This object cannot be garbage.
  GAB_OBJ_BLACK(obj);
}

static inline void inc_if_obj_ref(gab_value val) {
  if (GAB_VAL_IS_OBJ(val)) {
    inc_obj_ref(GAB_VAL_TO_OBJ(val));
  }
}

static inline void increment_stack(gab_engine *self) {
  if (!self->vm.frame)
    return;

  gab_value *stack_top = self->vm.top;

  gab_value *tracker = stack_top - 1;
  while (tracker >= self->vm.stack) {
    inc_if_obj_ref(*tracker--);
  }
}

static inline void decrement_stack(gab_engine *self) {
  if (!self->vm.frame)
    return;

  gab_value *stack_top = self->vm.top;

  gab_value *tracker = stack_top - 1;

  while (tracker >= self->vm.stack) {
    gab_engine_val_dref(self, *tracker--);
  }
}

static inline void process_increments(gab_engine *self) {
  while (self->gc.increment_count-- > 0) {
    inc_obj_ref(self->gc.increments[self->gc.increment_count]);
  }
}

static inline void process_decrements(gab_engine *self) {
  while (self->gc.decrement_count-- > 0) {
    dec_obj_ref(self, self->gc.decrements[self->gc.decrement_count]);
  }
}

static inline void mark_gray(gab_obj *obj);

static inline void dec_and_mark_gray(gab_obj *child) {
  if (child) {
    child->references -= 1;
    if (!GAB_OBJ_IS_GRAY(child)) {
      mark_gray(child);
    }
  }
}

static inline void mark_gray(gab_obj *obj) {
  if (!GAB_OBJ_IS_GRAY(obj)) {
    GAB_OBJ_GRAY(obj);
    for_child_do(obj, &dec_and_mark_gray);
  }
}

static inline void mark_roots(gab_engine *self) {
  for (i32 i = 0; i < self->gc.queue.cap; i++) {
    if (d_u64_istatus(&self->gc.queue, i) == D_FULL) {
      gab_value key = d_u64_ikey(&self->gc.roots, i);
      gab_obj *obj = GAB_VAL_TO_OBJ(key);

      if (GAB_OBJ_IS_PURPLE(obj) && obj->references > 0) {
        mark_gray(obj);
      } else {
        GAB_OBJ_NOT_BUFFERED(obj);

        d_u64_remove(&self->gc.roots, key);
        if (GAB_OBJ_IS_BLACK(obj) && obj->references == 0) {
          queue_destroy(self, obj);
        }
      }
    }
  }
}

static inline void scan_root_black(gab_obj *obj);
static inline void inc_and_scan_black(gab_obj *child) {
  child->references++;
  if (!GAB_OBJ_IS_BLACK(child))
    scan_root_black(child);
}

static inline void scan_root_black(gab_obj *obj) {
  GAB_OBJ_BLACK(obj);
  for_child_do(obj, &inc_and_scan_black);
}

static inline void scan_root(gab_obj *obj) {
  if (GAB_OBJ_IS_GRAY(obj)) {
    if (obj->references > 0) {
      scan_root_black(obj);
    } else {
      GAB_OBJ_WHITE(obj);
      for_child_do(obj, &scan_root);
    }
  }
}

static inline void scan_roots(gab_engine *self) {
  for (i32 i = 0; i < self->gc.queue.cap; i++) {
    if (d_u64_istatus(&self->gc.queue, i) == D_FULL) {
      gab_obj *obj = GAB_VAL_TO_OBJ(d_u64_ikey(&self->gc.roots, i));
      scan_root(obj);
    }
  }
}

static inline void collect_white(gab_engine *self, gab_obj *obj) {

  if (GAB_OBJ_IS_WHITE(obj) && !GAB_OBJ_IS_BUFFERED(obj)) {
    GAB_OBJ_BLACK(obj);

    for_child_and_gc_do(self, obj, collect_white);
    queue_destroy(self, obj);
  }
}

// Collecting roots is putting me in an infinte loop somehow
static inline void collect_roots(gab_engine *self) {
  for (i32 i = 0; i < self->gc.queue.cap; i++) {
    if (d_u64_istatus(&self->gc.queue, i) == D_FULL) {
      gab_value key = d_u64_ikey(&self->gc.roots, i);
      gab_obj *obj = GAB_VAL_TO_OBJ(key);

      d_u64_remove(&self->gc.roots, key);
      GAB_OBJ_NOT_BUFFERED(obj);

      collect_white(self, obj);
    }
  }
  self->gc.root_count = 0;
}

void gab_engine_collect_cycles(gab_engine *self) {
  mark_roots(self);
  scan_roots(self);
  collect_roots(self);
}

void gab_engine_collect(gab_engine *self) {
  increment_stack(self);
  process_increments(self);
  process_decrements(self);
  decrement_stack(self);
  trigger_destroy(self);
}
