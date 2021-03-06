#include "../compiler/engine.h"
#include <stdio.h>
#include <sys/types.h>

void gab_vm_create(gab_vm *self) {
  self->open_upvalues = NULL;
  self->frame = self->call_stack;
  self->top = self->stack;
}

void dump_frame(gab_engine *gab, gab_value *top, const char *name) {
  printf("Frame at %s:\n-----------------\n", name);
  gab_value *tracker = top - 1;
  while (tracker >= gab->vm.frame->slots) {
    printf("|%lu|", tracker - gab->vm.frame->slots);
    gab_val_dump(*tracker--);
    printf("\n");
  }
  printf("---------------\n");
};

void dump_stack(gab_engine *gab, gab_value *top, const char *name) {
  printf("Stack at %s:\n-----------------\n", name);
  gab_value *tracker = top - 1;
  while (tracker >= gab->vm.stack) {
    printf("|");
    gab_val_dump(*tracker--);
    printf("\n");
  }
  printf("---------------\n");
}

static inline gab_value *trim_return(gab_vm *restrict vm,
                                     gab_value *restrict from, gab_value *to,
                                     u8 have, u8 want) {
  u8 nulls = have == 0;

  if ((have != want) && (want != VAR_RET)) {
    if (have > want)
      have = want;
    else
      nulls = want - have;
  }

  const u8 got = have + nulls;

  while (have--)
    *to++ = *from++;

  while (nulls--)
    *to++ = GAB_VAL_NULL();

  *to = got;
  return to;
}

static inline gab_obj_upvalue *capture_upvalue(gab_engine *eng,
                                               gab_value *local) {
  gab_obj_upvalue *prev_upvalue = NULL;
  gab_obj_upvalue *upvalue = eng->vm.open_upvalues;

  while (upvalue != NULL && upvalue->data > local) {
    prev_upvalue = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->data == local) {
    return upvalue;
  }

  // The captured value's reference count has to be incremented
  // Before the allocation which creates the new upvalue.
  // This occurs in 'capture_upvalue'
  gab_engine_val_iref(eng, *local);

  gab_obj_upvalue *new_upvalue = gab_obj_upvalue_create(eng, local);

  new_upvalue->next = upvalue;
  if (prev_upvalue == NULL) {
    eng->vm.open_upvalues = new_upvalue;
  } else {
    prev_upvalue->next = new_upvalue;
  }
  return new_upvalue;
}

static inline void close_upvalue(gab_vm *vm, gab_value *local) {
  while (vm->open_upvalues != NULL && vm->open_upvalues->data >= local) {
    gab_obj_upvalue *upvalue = vm->open_upvalues;
    upvalue->closed = *upvalue->data;
    upvalue->data = &upvalue->closed;
    vm->open_upvalues = upvalue->next;
  }
}

gab_result *gab_engine_run(gab_engine *eng, gab_obj_closure *main) {
#if GAB_LOG_EXECUTION
  /*
    If we're logging execution, create the instruction name table and the
    log macro.
  */
  static const char *instr_name[] = {
#define OP_CODE(name) #name,
#include "../compiler/bytecode.h"
#undef OP_CODE
  };

#define LOG() printf("OP_%s\n", instr_name[*(ip)])

#else
#define LOG()
#endif

  /*
    If we're using computed gotos, create the jump table and the dispatch
    instructions.
  */
  static const void *dispatch_table[256] = {
#define OP_CODE(name) &&code_##name,
#include "../compiler/bytecode.h"
#undef OP_CODE
  };

#define CASE_CODE(name) code_##name
#define LOOP()
#define NEXT()                                                                 \
  ({                                                                           \
    LOG();                                                                     \
    goto *dispatch_table[*IP()];                                               \
  })

  /*
     We do a plus because it is slightly faster than an or.
     It is faster because there is no reliable branch to predict,
     and short circuiting can't help us.
  */
#define BINARY_OPERATION(value_type, operation_type, operation)                \
  if ((GAB_VAL_IS_NUMBER(PEEK()) + GAB_VAL_IS_NUMBER(PEEK2())) < 2) {          \
    STORE_FRAME();                                                             \
    return gab_run_fail(VM(), "Binary operations only work on numbers");       \
  }                                                                            \
  operation_type b = GAB_VAL_TO_NUMBER(POP());                                 \
  operation_type a = GAB_VAL_TO_NUMBER(POP());                                 \
  PUSH(value_type(a operation b));

/*
  Lots of helper macros.
*/
#define ENGINE() (eng)
#define VM() (&ENGINE()->vm)
#define GC() (&ENGINE()->gc)
#define FRAME() (VM()->frame)
#define CLOSURE() (FRAME()->closure)
#define IP() (ip)
#define TOP() (VM()->top)
#define SLOTS() (slots)
#define LOCAL(i) (SLOTS()[i])
#define UPVALUE(i) (*(GAB_VAL_TO_UPVALUE(CLOSURE()->upvalues[i])->data))
#define CONST_UPVALUE(i) (CLOSURE()->upvalues[i])

#define PUSH(value) (*TOP()++ = value)
#define POP() (*(--TOP()))
#define DROP() (TOP()--)
#define POP_N(n) (TOP() -= n)
#define DROP_N(n) (TOP() -= n)
#define PEEK() (*(TOP() - 1))
#define PEEK2() (*(TOP() - 2))
#define PEEK_N(n) (*(TOP() - n))

#define READ_BYTE (*IP()++)
#define READ_SHORT (IP() += 2, (((u16)IP()[-2] << 8) | IP()[-1]))
#define WRITE_SHORT(n) ((IP()[-2] = ((n) >> 8) & 0xff), IP()[-1] = (n)&0xff)
#define READ_INLINECACHE(type) (IP() += 8, (type **)(IP() - 8))

#define READ_CONSTANT (d_gab_value_ikey(ENGINE()->constants, (READ_SHORT)))
#define READ_STRING (GAB_VAL_TO_STRING(READ_CONSTANT))

#define STORE_FRAME() ({ VM()->frame->ip = IP(); })

#define LOAD_FRAME()                                                           \
  ({                                                                           \
    IP() = FRAME()->ip;                                                        \
    SLOTS() = FRAME()->slots;                                                  \
  })

  /*
   ----------- BEGIN RUN BODY -----------
  */

  gab_vm_create(VM());

  register u8 *ip;
  // register gab_call_frame *frame = VM()->frame;
  register gab_value *slots = TOP();

  PUSH(GAB_VAL_OBJ(main));
  PUSH(ENGINE()->std);

  gab_engine_obj_dref(ENGINE(), (gab_obj *)main);

  LOOP() {
    {
      u8 arity, want;
      gab_value callee;

      arity = 1;
      want = 1;
      callee = PEEK2();
      goto complete_call;

      CASE_CODE(VARCALL) : {
        u8 instr = *IP()++;
        u8 addtl = READ_BYTE;

        want = READ_BYTE;
        arity = *TOP() + addtl;
        callee = PEEK_N(arity - 1);

        goto complete_call;
      }

      // clang-format off
      CASE_CODE(CALL_0):
      CASE_CODE(CALL_1):
      CASE_CODE(CALL_2):
      CASE_CODE(CALL_3):
      CASE_CODE(CALL_4):
      CASE_CODE(CALL_5):
      CASE_CODE(CALL_6):
      CASE_CODE(CALL_7):
      CASE_CODE(CALL_8):
      CASE_CODE(CALL_9):
      CASE_CODE(CALL_10):
      CASE_CODE(CALL_11):
      CASE_CODE(CALL_12):
      CASE_CODE(CALL_13):
      CASE_CODE(CALL_14):
      CASE_CODE(CALL_15):
      CASE_CODE(CALL_16): {
        u8 instr = *IP()++;
        want = READ_BYTE;

        arity = instr - OP_CALL_0;
        callee = PEEK_N(arity - 1);

        goto complete_call;
      }
      // clang-format on

    complete_call : {
      STORE_FRAME();

      if (GAB_VAL_IS_CLOSURE(callee)) {
        gab_obj_closure *closure = GAB_VAL_TO_CLOSURE(callee);

        // Combine the two error branches into a single case.
        // Check for stack overflow here
        if (arity != closure->func->narguments) {
          return gab_run_fail(VM(), "Wrong number of arguments");
        }

        FRAME()++;
        FRAME()->closure = closure;
        FRAME()->ip =
            CLOSURE()->func->module->bytecode.data + CLOSURE()->func->offset;
        FRAME()->slots = TOP() - arity - 1;
        FRAME()->expected_results = want;

        TOP() += CLOSURE()->func->nlocals - 1;

        LOAD_FRAME();

      } else if (GAB_VAL_IS_BUILTIN(callee)) {
        gab_obj_builtin *builtin = GAB_VAL_TO_BUILTIN(callee);

        if (arity != builtin->narguments && builtin->narguments != VAR_RET) {
          return gab_run_fail(VM(), "Wrong number of arguments");
        }

        gab_value result = (*builtin->function)(ENGINE(), TOP() - arity, arity);

        TOP() -= arity + 1;
        u8 have = 1;

        TOP() = trim_return(VM(), &result, TOP(), have, want);
      } else {
        return gab_run_fail(VM(), "Expected a callable");
      }

      NEXT();
    }
    }

    {

      u8 have, addtl;

      CASE_CODE(VARRETURN) : {
        u8 instr = *IP()++;
        addtl = READ_BYTE;
        have = *TOP() + addtl;
        goto complete_return;
      }

      // clang-format off
      CASE_CODE(RETURN_1):
      CASE_CODE(RETURN_2):
      CASE_CODE(RETURN_3):
      CASE_CODE(RETURN_4):
      CASE_CODE(RETURN_5):
      CASE_CODE(RETURN_6):
      CASE_CODE(RETURN_7):
      CASE_CODE(RETURN_8):
      CASE_CODE(RETURN_9):
      CASE_CODE(RETURN_10):
      CASE_CODE(RETURN_11):
      CASE_CODE(RETURN_12):
      CASE_CODE(RETURN_13):
      CASE_CODE(RETURN_14):
      CASE_CODE(RETURN_15):
      CASE_CODE(RETURN_16): {
        u8 instr = *IP()++;
        have = instr - OP_RETURN_1 + 1;
        goto complete_return;
      }
      // clang-format on

    complete_return : {

      gab_value *from = TOP() - have;

      close_upvalue(VM(), FRAME()->slots);

      // gab_value result = POP();
      // TOP() = FRAME()->slots;
      // PUSH(result);

      TOP() = trim_return(VM(), from, FRAME()->slots, have,
                          FRAME()->expected_results);

      if (--FRAME() == VM()->call_stack) {
        FRAME()++;
        // Increment and pop the module.
        gab_engine_val_iref(ENGINE(), PEEK());

        gab_value module = POP();

        return gab_run_success(module);
      }

      LOAD_FRAME();
      NEXT();
    }
    }

    {
      gab_value prop, index;
      i16 prop_offset;
      gab_obj_object *obj;

      CASE_CODE(GET_INDEX) : {
        u8 instr = *IP()++;
        prop = PEEK();
        index = PEEK2();

        if (!GAB_VAL_IS_OBJECT(index)) {
          STORE_FRAME();
          return gab_run_fail(VM(), "Only objects have properties");
        }

        obj = GAB_VAL_TO_OBJECT(index);

        prop_offset = gab_obj_shape_find(obj->shape, prop);

        DROP_N(2);
        goto complete_obj_get;
      }

      CASE_CODE(GET_PROPERTY) : {
        u8 instr = *IP()++;
        prop = READ_CONSTANT;
        index = PEEK();

        gab_obj_shape **cached_shape = READ_INLINECACHE(gab_obj_shape);
        prop_offset = READ_SHORT;

        if (!GAB_VAL_IS_OBJECT(index)) {
          STORE_FRAME();
          return gab_run_fail(VM(), "Only objects have properties");
        }

        obj = GAB_VAL_TO_OBJECT(index);

        if (*cached_shape != obj->shape) {
          // The cache hasn't been created yet.
          *cached_shape = obj->shape;
          prop_offset = gab_obj_shape_find(obj->shape, prop);
          // Writes into the short just before the ip.
          WRITE_SHORT(prop_offset);
        }

        DROP();
        goto complete_obj_get;
      }

    complete_obj_get : {
      PUSH(gab_obj_object_get(obj, prop_offset));
      NEXT();
    }
    }

    {

      gab_value value, prop, index;
      i16 prop_offset;
      gab_obj_object *obj;

      CASE_CODE(SET_INDEX) : {
        u8 instr = *IP()++;
        // Leave these on the stack for now in case we collect.
        value = PEEK();
        prop = PEEK2();
        index = PEEK_N(3);

        gab_engine_val_iref(ENGINE(), value);

        if (!GAB_VAL_IS_OBJECT(index)) {
          STORE_FRAME();
          return gab_run_fail(VM(), "Only objects have properties");
        }

        obj = GAB_VAL_TO_OBJECT(index);

        prop_offset = gab_obj_shape_find(obj->shape, prop);

        if (prop_offset < 0) {
          // The key didn't exist on the old shape.
          // Create a new shape and update the cache.
          gab_obj_shape *shape =
              gab_obj_shape_extend(obj->shape, ENGINE(), prop);

          // Update the obj and get the new offset.
          prop_offset = gab_obj_object_extend(obj, ENGINE(), shape, value);
        }

        // Now its safe to drop the three values
        DROP_N(3);
        goto complete_obj_set;
      }

      CASE_CODE(SET_PROPERTY) : {
        u8 instr = *IP()++;
        prop = READ_CONSTANT;
        value = PEEK();
        index = PEEK2();

        gab_obj_shape **cached_shape = READ_INLINECACHE(gab_obj_shape);

        prop_offset = READ_SHORT;

        if (!GAB_VAL_IS_OBJECT(index)) {
          STORE_FRAME();
          return gab_run_fail(VM(), "Only objects have properties");
        }

        obj = GAB_VAL_TO_OBJECT(index);

        if (*cached_shape != obj->shape) {
          // The cache hasn't been created yet.
          *cached_shape = obj->shape;

          prop_offset = gab_obj_shape_find(obj->shape, prop);
          // Writes into the short just before the ip.
          WRITE_SHORT(prop_offset);
        }

        if (prop_offset < 0) {
          // The key didn't exist on the old shape.
          // Create a new shape and update the cache.
          *cached_shape = gab_obj_shape_extend(*cached_shape, ENGINE(), prop);

          // Update the obj and get the new offset.
          prop_offset =
              gab_obj_object_extend(obj, ENGINE(), *cached_shape, value);

          // Write the offset.
          WRITE_SHORT(prop_offset);
        }

        DROP_N(2);
        goto complete_obj_set;
      }

    complete_obj_set : {

      gab_engine_val_iref(ENGINE(), value);
      gab_engine_val_dref(ENGINE(), gab_obj_object_get(obj, prop_offset));

      gab_obj_object_set(obj, prop_offset, value);

      PUSH(value);

      NEXT();
    }
    }

    CASE_CODE(CONSTANT) : {
      u8 instr = *IP()++;
      PUSH(READ_CONSTANT);
      NEXT();
    }

    CASE_CODE(NEGATE) : {
      u8 instr = *IP()++;
      if (!GAB_VAL_IS_NUMBER(PEEK())) {
        STORE_FRAME();
        return gab_run_fail(VM(), "Can only negate numbers.");
      }
      gab_value num = GAB_VAL_NUMBER(-GAB_VAL_TO_NUMBER(POP()));
      PUSH(num);
      NEXT();
    }

    CASE_CODE(SUBTRACT) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_NUMBER, f64, -);
      NEXT();
    }

    CASE_CODE(ADD) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_NUMBER, f64, +);
      NEXT();
    }

    CASE_CODE(DIVIDE) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_NUMBER, f64, /);
      NEXT();
    }

    CASE_CODE(MODULO) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_NUMBER, u64, %);
      NEXT();
    }

    CASE_CODE(MULTIPLY) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_NUMBER, f64, *);
      NEXT();
    }

    CASE_CODE(GREATER) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_BOOLEAN, f64, >);
      NEXT();
    }

    CASE_CODE(GREATER_EQUAL) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_BOOLEAN, f64, >=);
      NEXT();
    }

    CASE_CODE(LESSER) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_BOOLEAN, f64, <);
      NEXT();
    }

    CASE_CODE(LESSER_EQUAL) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_BOOLEAN, f64, <=);
      NEXT();
    }

    CASE_CODE(BITWISE_AND) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_NUMBER, u64, &);
      NEXT();
    }

    CASE_CODE(BITWISE_OR) : {
      u8 instr = *IP()++;
      BINARY_OPERATION(GAB_VAL_NUMBER, u64, |);
      NEXT();
    }

    CASE_CODE(EQUAL) : {
      u8 instr = *IP()++;
      gab_value a = POP();
      gab_value b = POP();
      PUSH(GAB_VAL_BOOLEAN(a == b));
      NEXT();
    }

    CASE_CODE(SWAP) : {
      u8 instr = *IP()++;
      gab_value tmp = PEEK();
      PEEK() = PEEK2();
      PEEK2() = tmp;
      NEXT();
    }

    CASE_CODE(DUP) : {
      u8 instr = *IP()++;
      gab_value peek = PEEK();
      PUSH(peek);
      NEXT();
    }

    CASE_CODE(CONCAT) : {
      u8 instr = *IP()++;
      if (!GAB_VAL_IS_STRING(PEEK()) + !GAB_VAL_IS_STRING(PEEK2())) {
        STORE_FRAME();
        return gab_run_fail(VM(), "Can only concatenate strings");
      }

      gab_obj_string *b = GAB_VAL_TO_STRING(POP());
      gab_obj_string *a = GAB_VAL_TO_STRING(POP());

      gab_obj_string *obj = gab_obj_string_concat(ENGINE(), a, b);

      PUSH(GAB_VAL_OBJ(obj));

      NEXT();
    }

    CASE_CODE(STRINGIFY) : {
      u8 instr = *IP()++;
      if (!GAB_VAL_IS_STRING(PEEK())) {
        gab_obj_string *obj = gab_val_to_obj_string(POP(), ENGINE());

        PUSH(GAB_VAL_OBJ(obj));
      }
      NEXT();
    }

    CASE_CODE(LOGICAL_AND) : {
      u8 instr = *IP()++;
      u16 offset = READ_SHORT;
      gab_value cond = PEEK();

      if (gab_val_falsey(cond)) {
        ip += offset;
      } else {
        DROP();
      }

      NEXT();
    }

    CASE_CODE(LOGICAL_OR) : {
      u8 instr = *IP()++;
      u16 offset = READ_SHORT;
      gab_value cond = PEEK();

      if (gab_val_falsey(cond)) {
        DROP();
      } else {
        ip += offset;
      }

      NEXT();
    }

    CASE_CODE(PUSH_NULL) : {
      u8 instr = *IP()++;
      PUSH(GAB_VAL_NULL());
      NEXT();
    }

    CASE_CODE(PUSH_TRUE) : {
      u8 instr = *IP()++;
      PUSH(GAB_VAL_BOOLEAN(true));
      NEXT();
    }

    CASE_CODE(PUSH_FALSE) : {
      u8 instr = *IP()++;
      PUSH(GAB_VAL_BOOLEAN(false));
      NEXT();
    }

    CASE_CODE(NOT) : {
      u8 instr = *IP()++;
      gab_value val = GAB_VAL_BOOLEAN(gab_val_falsey(POP()));
      PUSH(val);
      NEXT();
    }

    CASE_CODE(ASSERT) : {
      u8 instr = *IP()++;
      if (GAB_VAL_IS_NULL(PEEK())) {
        STORE_FRAME();
        return gab_run_fail(VM(), "Expected value to not be null");
      }
      NEXT();
    }

    CASE_CODE(TYPE) : {
      u8 instr = *IP()++;
      PEEK() = gab_val_type(ENGINE(), PEEK());
      NEXT();
    }

    CASE_CODE(MATCH) : {
      u8 instr = *IP()++;
      gab_value test = POP();
      gab_value pattern = PEEK();
      if (test == pattern) {
        POP();
        PUSH(GAB_VAL_BOOLEAN(true));
      } else {
        PUSH(GAB_VAL_BOOLEAN(false));
      }
      NEXT();
    }

    CASE_CODE(SPREAD) : {
      u8 instr = *IP()++;
      u8 want = READ_BYTE;

      gab_value index = POP();

      if (GAB_VAL_IS_OBJECT(index)) {
        gab_obj_object *obj = GAB_VAL_TO_OBJECT(index);

        u8 have = obj->is_dynamic ? obj->dynamic_values.len : obj->static_size;

        TOP() = trim_return(VM(),
                            obj->is_dynamic ? obj->dynamic_values.data
                                            : obj->static_values,
                            TOP(), have, want);
      } else if (GAB_VAL_IS_SHAPE(index)) {
        gab_obj_shape *shape = GAB_VAL_TO_SHAPE(index);

        u8 have = shape->properties.len;

        TOP() = trim_return(VM(), shape->keys, TOP(), have, want);
      } else {
        STORE_FRAME();
        return gab_run_fail(VM(), "Spread operator only works on objects");
      }

      NEXT();
    }

    CASE_CODE(POP) : {
      u8 instr = *IP()++;
      DROP();
      NEXT();
    }

    CASE_CODE(POP_N) : {
      u8 instr = *IP()++;
      DROP_N(READ_BYTE);
      NEXT();
    }

    // clang-format off

    CASE_CODE(LOAD_LOCAL_0):
    CASE_CODE(LOAD_LOCAL_1):
    CASE_CODE(LOAD_LOCAL_2):
    CASE_CODE(LOAD_LOCAL_3):
    CASE_CODE(LOAD_LOCAL_4):
    CASE_CODE(LOAD_LOCAL_5):
    CASE_CODE(LOAD_LOCAL_6):
    CASE_CODE(LOAD_LOCAL_7):
    CASE_CODE(LOAD_LOCAL_8): {
      u8 instr = *IP()++;
      PUSH(LOCAL(instr- OP_LOAD_LOCAL_0));
      NEXT();
    }

    CASE_CODE(STORE_LOCAL_0):
    CASE_CODE(STORE_LOCAL_1):
    CASE_CODE(STORE_LOCAL_2):
    CASE_CODE(STORE_LOCAL_3):
    CASE_CODE(STORE_LOCAL_4):
    CASE_CODE(STORE_LOCAL_5):
    CASE_CODE(STORE_LOCAL_6):
    CASE_CODE(STORE_LOCAL_7):
    CASE_CODE(STORE_LOCAL_8): {
      u8 instr = *IP()++;
      LOCAL(instr - OP_STORE_LOCAL_0) = PEEK();
      NEXT();
    }

    CASE_CODE(LOAD_UPVALUE_0) :
    CASE_CODE(LOAD_UPVALUE_1) :
    CASE_CODE(LOAD_UPVALUE_2) :
    CASE_CODE(LOAD_UPVALUE_3) :
    CASE_CODE(LOAD_UPVALUE_4) :
    CASE_CODE(LOAD_UPVALUE_5) :
    CASE_CODE(LOAD_UPVALUE_6) :
    CASE_CODE(LOAD_UPVALUE_7) :
    CASE_CODE(LOAD_UPVALUE_8) : {
      u8 instr = *IP()++;
      PUSH(UPVALUE(instr - OP_LOAD_UPVALUE_0));
      NEXT();
    }

    CASE_CODE(STORE_UPVALUE_0) :
    CASE_CODE(STORE_UPVALUE_1) :
    CASE_CODE(STORE_UPVALUE_2) :
    CASE_CODE(STORE_UPVALUE_3) :
    CASE_CODE(STORE_UPVALUE_4) :
    CASE_CODE(STORE_UPVALUE_5) :
    CASE_CODE(STORE_UPVALUE_6) :
    CASE_CODE(STORE_UPVALUE_7) :
    CASE_CODE(STORE_UPVALUE_8) : {
      u8 instr = *IP()++;
      UPVALUE(instr - OP_STORE_UPVALUE_0) = PEEK();
      NEXT();
    }

    CASE_CODE(LOAD_CONST_UPVALUE_0) :
    CASE_CODE(LOAD_CONST_UPVALUE_1) :
    CASE_CODE(LOAD_CONST_UPVALUE_2) :
    CASE_CODE(LOAD_CONST_UPVALUE_3) :
    CASE_CODE(LOAD_CONST_UPVALUE_4) :
    CASE_CODE(LOAD_CONST_UPVALUE_5) :
    CASE_CODE(LOAD_CONST_UPVALUE_6) :
    CASE_CODE(LOAD_CONST_UPVALUE_7) :
    CASE_CODE(LOAD_CONST_UPVALUE_8) : {
      u8 instr = *IP()++;
      PUSH(CONST_UPVALUE(instr - OP_LOAD_CONST_UPVALUE_0));
      NEXT();
    }
    // clang-format on

    CASE_CODE(LOAD_LOCAL) : {
      u8 instr = *IP()++;
      PUSH(LOCAL(READ_BYTE));
      NEXT();
    }

    CASE_CODE(STORE_LOCAL) : {
      u8 instr = *IP()++;
      LOCAL(READ_BYTE) = PEEK();
      NEXT();
    }

    CASE_CODE(POP_STORE_LOCAL) : {
      u8 instr = *IP()++;
      LOCAL(READ_BYTE) = POP();
      NEXT();
    }

    CASE_CODE(LOAD_UPVALUE) : {
      u8 instr = *IP()++;
      PUSH(UPVALUE(READ_BYTE));
      NEXT();
    }

    CASE_CODE(LOAD_CONST_UPVALUE) : {
      u8 instr = *IP()++;
      PUSH(CONST_UPVALUE(READ_BYTE));
      NEXT();
    }

    CASE_CODE(STORE_UPVALUE) : {
      u8 instr = *IP()++;
      UPVALUE(READ_BYTE) = PEEK();
      NEXT();
    }

    CASE_CODE(POP_STORE_UPVALUE) : {
      u8 instr = *IP()++;
      UPVALUE(READ_BYTE) = POP();
      NEXT();
    }

    CASE_CODE(CLOSE_UPVALUE) : {
      u8 instr = *IP()++;
      close_upvalue(VM(), SLOTS() + READ_BYTE);
      NEXT();
    }

    CASE_CODE(POP_JUMP_IF_TRUE) : {
      u8 instr = *IP()++;
      u16 dist = READ_SHORT;
      ip += dist * !gab_val_falsey(POP());
      NEXT();
    }

    CASE_CODE(POP_JUMP_IF_FALSE) : {
      u8 instr = *IP()++;
      u16 dist = READ_SHORT;
      ip += dist * gab_val_falsey(POP());
      NEXT();
    }

    CASE_CODE(JUMP_IF_TRUE) : {
      u8 instr = *IP()++;
      u16 dist = READ_SHORT;
      ip += dist * !gab_val_falsey(PEEK());
      NEXT();
    }

    CASE_CODE(JUMP_IF_FALSE) : {
      u8 instr = *IP()++;
      u16 dist = READ_SHORT;
      ip += dist * gab_val_falsey(PEEK());
      NEXT();
    }

    CASE_CODE(JUMP) : {
      u8 instr = *IP()++;
      u16 dist = READ_SHORT;
      ip += dist;
      NEXT();
    }

    CASE_CODE(LOOP) : {
      u8 instr = *IP()++;
      u16 dist = READ_SHORT;
      ip -= dist;
      NEXT();
    }

    CASE_CODE(CLOSURE) : {
      u8 instr = *IP()++;
      gab_obj_function *func = GAB_VAL_TO_FUNCTION(READ_CONSTANT);

      gab_value upvalues[UPVALUE_MAX];

      for (int i = 0; i < func->nupvalues; i++) {
        u8 flags = READ_BYTE;
        u8 index = READ_BYTE;

        if (flags & FLAG_LOCAL) {
          if (flags & FLAG_MUTABLE) {
            upvalues[i] =
                GAB_VAL_OBJ(capture_upvalue(ENGINE(), FRAME()->slots + index));
          } else {
            upvalues[i] = LOCAL(index);
            gab_engine_val_iref(ENGINE(), upvalues[i]);
          }
        } else {
          upvalues[i] = CLOSURE()->upvalues[index];
          gab_engine_val_iref(ENGINE(), upvalues[i]);
        }
      }

      gab_obj_closure *obj = gab_obj_closure_create(ENGINE(), func, upvalues);

      PUSH(GAB_VAL_OBJ(obj));

      gab_engine_obj_dref(ENGINE(), (gab_obj *)obj);

      NEXT();
    }

    {
      gab_obj_shape *shape;
      u8 size;

      CASE_CODE(OBJECT_RECORD_DEF) : {
        u8 instr = *IP()++;
        gab_obj_string *name = READ_STRING;

        size = READ_BYTE;

        for (u64 i = 0; i < size * 2; i++) {
          gab_engine_val_iref(ENGINE(), PEEK_N(i));
        }

        shape = gab_obj_shape_create(ENGINE(), TOP() - (size * 2), size, 2);

        shape->name = gab_obj_string_ref(name);

        goto complete_shape;
      }

      CASE_CODE(OBJECT_RECORD) : {
        u8 instr = *IP()++;
        size = READ_BYTE;

        for (u64 i = 0; i < size * 2; i++) {
          gab_engine_val_iref(ENGINE(), PEEK_N(i));
        }

        shape = gab_obj_shape_create(ENGINE(), TOP() - (size * 2), size, 2);

        goto complete_shape;
      }

    complete_shape : {
      gab_obj_object *obj = gab_obj_object_create(
          ENGINE(), shape, TOP() + 1 - (size * 2), size, 2);

      DROP_N(size * 2);

      PUSH(GAB_VAL_OBJ(obj));

      gab_engine_obj_dref(ENGINE(), (gab_obj *)obj);

      NEXT();
    }
    }

    CASE_CODE(OBJECT_ARRAY) : {
      u8 instr = *IP()++;
      u8 size = READ_BYTE;

      for (u64 i = 0; i < size; i++) {
        gab_engine_val_iref(ENGINE(), PEEK_N(i));
      }

      gab_obj_shape *shape = gab_obj_shape_create_array(ENGINE(), size);

      gab_obj_object *obj =
          gab_obj_object_create(ENGINE(), shape, TOP() - (size), size, 1);

      DROP_N(size);

      PUSH(GAB_VAL_OBJ(obj));

      gab_engine_obj_dref(ENGINE(), (gab_obj *)obj);

      NEXT();
    }
  }
  // This should be unreachable
  return NULL;
}

#undef READ_BYTE
#undef READ_CONSTANT
#undef READ_SHORT
#undef READ_STRING
#undef BINARY_OPERATION
