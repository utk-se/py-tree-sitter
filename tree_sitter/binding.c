#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include <wctype.h>
#include "tree_sitter/api.h"

// Types

typedef struct {
  PyObject_HEAD
  TSNode node;
  PyObject *children;
  PyObject *tree;
} Node;

typedef struct {
  PyObject_HEAD
  TSTree *tree;
  PyObject *source;
  int edited;
} Tree;

typedef struct {
  PyObject_HEAD
  TSParser *parser;
} Parser;

typedef struct {
  PyObject_HEAD
  TSTreeCursor cursor;
  PyObject *node;
  PyObject *tree;
} TreeCursor;

typedef struct {
  PyObject_HEAD
  TSQuery *query;
  PyObject *capture_names;
} Query;

static TSTreeCursor default_cursor = {0};
static TSQueryCursor *query_cursor = NULL;

// Point

static PyObject *point_new(TSPoint point) {
  PyObject *row = PyLong_FromSize_t((size_t)point.row);
  PyObject *column = PyLong_FromSize_t((size_t)point.column);
  if (!row || !column) {
    Py_XDECREF(row);
    Py_XDECREF(column);
    return NULL;
  }

  PyObject *obj = PyTuple_Pack(2, row, column);
  Py_XDECREF(row);
  Py_XDECREF(column);
  return obj;
}

// Node

static PyObject *node_new_internal(TSNode node, PyObject *tree);
static PyObject *tree_cursor_new_internal(TSNode node, PyObject *tree);

static void node_dealloc(Node *self) {
  Py_XDECREF(self->children);
  Py_XDECREF(self->tree);
  Py_TYPE(self)->tp_free(self);
}

static PyObject *node_repr(Node *self) {
  const char *type = ts_node_type(self->node);
  TSPoint start_point = ts_node_start_point(self->node);
  TSPoint end_point = ts_node_end_point(self->node);
  const char *format_string = ts_node_is_named(self->node)
    ? "<Node kind=%s, start_point=(%u, %u), end_point=(%u, %u)>"
    : "<Node kind=\"%s\", start_point=(%u, %u), end_point=(%u, %u)>";
  return PyUnicode_FromFormat(
    format_string,
    type,
    start_point.row,
    start_point.column,
    end_point.row,
    end_point.column
  );
}

static bool node_is_instance(PyObject *self);

static PyObject *node_compare(Node *self, Node *other, int op) {
  if (node_is_instance((PyObject *)other)) {
    bool result = ts_node_eq(self->node, other->node);
    switch (op) {
      case Py_EQ: return PyBool_FromLong(result);
      case Py_NE: return PyBool_FromLong(!result);
      default: Py_RETURN_FALSE;
    }
  } else {
    Py_RETURN_FALSE;
  }
}

static PyObject *node_sexp(Node *self, PyObject *args) {
  char *string = ts_node_string(self->node);
  PyObject *result = PyUnicode_FromString(string);
  free(string);
  return result;
}

static PyObject *node_walk(Node *self, PyObject *args) {
  return tree_cursor_new_internal(self->node, self->tree);
}

static PyObject *node_chield_by_field_id(Node *self, PyObject *args) {
  TSFieldId field_id;
  if (!PyArg_ParseTuple(args, "H", &field_id)) {
    return NULL;
  }
  TSNode child = ts_node_child_by_field_id(self->node, field_id);
  if (ts_node_is_null(child)) {
    Py_RETURN_NONE;
  }
  return node_new_internal(child, self->tree);
}

static PyObject *node_chield_by_field_name(Node *self, PyObject *args) {
  char *name;
  Py_ssize_t length;
  if (!PyArg_ParseTuple(args, "s#", &name, &length)) {
    return NULL;
  }
  TSNode child = ts_node_child_by_field_name(self->node, name, length);
  if (ts_node_is_null(child)) {
    Py_RETURN_NONE;
  }
  return node_new_internal(child, self->tree);
}

static PyObject *node_get_type(Node *self, void *payload) {
  return PyUnicode_FromString(ts_node_type(self->node));
}

static PyObject *node_get_is_named(Node *self, void *payload) {
  return PyBool_FromLong(ts_node_is_named(self->node));
}

static PyObject *node_get_is_missing(Node *self, void *payload) {
  return PyBool_FromLong(ts_node_is_missing(self->node));
}

static PyObject *node_get_has_changes(Node *self, void *payload) {
  return PyBool_FromLong(ts_node_has_changes(self->node));
}

static PyObject *node_get_has_error(Node *self, void *payload) {
  return PyBool_FromLong(ts_node_has_error(self->node));
}

static PyObject *node_get_start_byte(Node *self, void *payload) {
  return PyLong_FromSize_t((size_t)ts_node_start_byte(self->node));
}

static PyObject *node_get_end_byte(Node *self, void *payload) {
  return PyLong_FromSize_t((size_t)ts_node_end_byte(self->node));
}

static PyObject *node_get_start_point(Node *self, void *payload) {
  return point_new(ts_node_start_point(self->node));
}

static PyObject *node_get_end_point(Node *self, void *payload) {
  return point_new(ts_node_end_point(self->node));
}

static PyObject *node_get_children(Node *self, void *payload) {
  if (self->children) {
    Py_INCREF(self->children);
    return self->children;
  }

  long length = (long)ts_node_child_count(self->node);
  PyObject *result = PyList_New(length);
  if (length > 0) {
    ts_tree_cursor_reset(&default_cursor, self->node);
    ts_tree_cursor_goto_first_child(&default_cursor);
    int i = 0;
    do {
      TSNode child = ts_tree_cursor_current_node(&default_cursor);
      PyList_SetItem(result, i, node_new_internal(child, self->tree));
      i++;
    } while (ts_tree_cursor_goto_next_sibling(&default_cursor));
  }
  Py_INCREF(result);
  self->children = result;
  return result;
}

static PyObject *node_get_child_count(Node *self, void *payload) {
  long length = (long)ts_node_child_count(self->node);
  PyObject *result = PyLong_FromLong(length);
  return result;
}

static PyObject *node_get_named_child_count(Node *self, void *payload) {
  long length = (long)ts_node_named_child_count(self->node);
  PyObject *result = PyLong_FromLong(length);
  return result;
}

static PyObject *node_get_next_sibling(Node *self, void *payload) {
  TSNode next_sibling = ts_node_next_sibling(self->node);
  if (ts_node_is_null(next_sibling)) {
    Py_RETURN_NONE;
  }
  return node_new_internal(next_sibling, self->tree);
}

static PyObject *node_get_prev_sibling(Node *self, void *payload) {
  TSNode prev_sibling = ts_node_prev_sibling(self->node);
  if (ts_node_is_null(prev_sibling)) {
    Py_RETURN_NONE;
  }
  return node_new_internal(prev_sibling, self->tree);
}

static PyObject *node_get_next_named_sibling(Node *self, void *payload) {
  TSNode next_named_sibling = ts_node_next_named_sibling(self->node);
  if (ts_node_is_null(next_named_sibling)) {
    Py_RETURN_NONE;
  }
  return node_new_internal(next_named_sibling, self->tree);
}

static PyObject *node_get_prev_named_sibling(Node *self, void *payload) {
  TSNode prev_named_sibling = ts_node_prev_named_sibling(self->node);
  if (ts_node_is_null(prev_named_sibling)) {
    Py_RETURN_NONE;
  }
  return node_new_internal(prev_named_sibling, self->tree);
}

static PyObject *node_get_parent(Node *self, void *payload) {
  TSNode parent = ts_node_parent(self->node);
  if (ts_node_is_null(parent)) {
    Py_RETURN_NONE;
  }
  return node_new_internal(parent, self->tree);
}

// forward declaraction
static PyObject *tree_get_text(Tree *self, void *payload);

static PyObject *node_get_text(Node *self, void *payload) {
  Tree *tree = (Tree *)self->tree;
  if (tree == NULL) {
    PyErr_SetString(PyExc_ValueError, "No tree");
    return NULL;
  }
  PyObject *source = tree_get_text(tree, NULL);
  if (source == Py_None) {
    Py_RETURN_NONE;
  }
  // "hello"[1:3] == "hello".__getitem__(slice(1, 3))
  PyObject *start_byte =
    PyLong_FromSize_t((size_t)ts_node_start_byte(self->node));
  if (start_byte == NULL) {
    PyErr_SetString(PyExc_RuntimeError,
                    "Failed to determine start byte");
    return NULL;
  }
  PyObject *end_byte =
    PyLong_FromSize_t((size_t)ts_node_end_byte(self->node));
  if (end_byte == NULL) {
    Py_DECREF(start_byte);
    PyErr_SetString(PyExc_RuntimeError,
                    "Failed to determine end byte");
    return NULL;
  }
  PyObject *slice = PySlice_New(start_byte, end_byte, NULL);
  Py_DECREF(start_byte);
  Py_DECREF(end_byte);
  if (slice == NULL) {
    PyErr_SetString(PyExc_RuntimeError,
                    "PySlice_New failed");
    return NULL;
  }
  PyObject *node_mv = PyMemoryView_FromObject(source);
  Py_DECREF(source);
  if (node_mv == NULL) {
    Py_DECREF(slice);
    PyErr_SetString(PyExc_RuntimeError,
                    "PyMemoryView_FromObject failed");
    return NULL;
  }
  PyObject *node_slice = PyObject_GetItem(node_mv, slice);
  Py_DECREF(slice);
  Py_DECREF(node_mv);
  if (node_slice == NULL) {
    PyErr_SetString(PyExc_RuntimeError,
                    "PyObject_GetItem failed");
    return NULL;
  }
  Py_INCREF(node_slice);
  return node_slice;
}

static PyMethodDef node_methods[] = {
  {
    .ml_name = "walk",
    .ml_meth = (PyCFunction)node_walk,
    .ml_flags = METH_NOARGS,
    .ml_doc = "walk()\n--\n\n\
               Get a tree cursor for walking the tree starting at this node.",
  },
  {
    .ml_name = "sexp",
    .ml_meth = (PyCFunction)node_sexp,
    .ml_flags = METH_NOARGS,
    .ml_doc = "sexp()\n--\n\n\
               Get an S-expression representing the node.",
  },
  {
    .ml_name = "child_by_field_id",
    .ml_meth = (PyCFunction)node_chield_by_field_id,
    .ml_flags = METH_VARARGS,
    .ml_doc = "child_by_field_id(id)\n--\n\n\
               Get child for the given field id.",
  },
  {
    .ml_name = "child_by_field_name",
    .ml_meth = (PyCFunction)node_chield_by_field_name,
    .ml_flags = METH_VARARGS,
    .ml_doc = "child_by_field_name(name)\n--\n\n\
               Get child for the given field name.",
  },
  {NULL},
};

static PyGetSetDef node_accessors[] = {
  {"type", (getter)node_get_type, NULL, "The node's type", NULL},
  {"is_named", (getter)node_get_is_named, NULL, "Is this a named node", NULL},
  {"is_missing", (getter)node_get_is_missing, NULL, "Is this a node inserted by the parser", NULL},
  {"has_changes", (getter)node_get_has_changes, NULL, "Does this node have text changes since it was parsed", NULL},
  {"has_error", (getter)node_get_has_error, NULL, "Does this node contain any errors", NULL},
  {"start_byte", (getter)node_get_start_byte, NULL, "The node's start byte", NULL},
  {"end_byte", (getter)node_get_end_byte, NULL, "The node's end byte", NULL},
  {"start_point", (getter)node_get_start_point, NULL, "The node's start point", NULL},
  {"end_point", (getter)node_get_end_point, NULL, "The node's end point", NULL},
  {"children", (getter)node_get_children, NULL, "The node's children", NULL},
  {"child_count", (getter)node_get_child_count, NULL, "The number of children for a node", NULL},
  {"named_child_count", (getter)node_get_named_child_count, NULL, "The number of named children for a node", NULL},
  {"next_sibling", (getter)node_get_next_sibling, NULL, "The node's next sibling", NULL},
  {"prev_sibling", (getter)node_get_prev_sibling, NULL, "The node's previous sibling", NULL},
  {"next_named_sibling", (getter)node_get_next_named_sibling, NULL, "The node's next named sibling", NULL},
  {"prev_named_sibling", (getter)node_get_prev_named_sibling, NULL, "The node's previous named sibling", NULL},
  {"parent", (getter)node_get_parent, NULL, "The node's parent", NULL},
  {"text", (getter)node_get_text, NULL, "The node's text, if tree has not been edited", NULL},
  {NULL}
};

static PyTypeObject node_type = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "tree_sitter.Node",
  .tp_doc = "A syntax node",
  .tp_basicsize = sizeof(Node),
  .tp_itemsize = 0,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_dealloc = (destructor)node_dealloc,
  .tp_repr = (reprfunc)node_repr,
  .tp_richcompare = (richcmpfunc)node_compare,
  .tp_methods = node_methods,
  .tp_getset = node_accessors,
};

static PyObject *node_new_internal(TSNode node, PyObject *tree) {
  Node *self = (Node *)node_type.tp_alloc(&node_type, 0);
  if (self != NULL) {
    self->node = node;
    Py_INCREF(tree);
    self->tree = tree;
    self->children = NULL;
  }
  return (PyObject *)self;
}

static bool node_is_instance(PyObject *self) {
  return PyObject_IsInstance(self, (PyObject *)&node_type);
}

// Tree

static void tree_dealloc(Tree *self) {
  ts_tree_delete(self->tree);
  Py_XDECREF(self->source);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *tree_get_root_node(Tree *self, void *payload) {
  return node_new_internal(ts_tree_root_node(self->tree), (PyObject *)self);
}

static PyObject *tree_get_text(Tree *self, void *payload) {
  if (self->edited) {
    Py_RETURN_NONE;
  }
  PyObject *source = self->source;
  if (source == NULL) {
    Py_RETURN_NONE;
  }
  Py_INCREF(source);
  return source;
}

static PyObject *tree_walk(Tree *self, PyObject *args) {
  return tree_cursor_new_internal(ts_tree_root_node(self->tree), (PyObject *)self);
}

static PyObject *tree_edit(Tree *self, PyObject *args, PyObject *kwargs) {
  unsigned start_byte, start_row, start_column;
  unsigned old_end_byte, old_end_row, old_end_column;
  unsigned new_end_byte, new_end_row, new_end_column;

  char *keywords[] = {
    "start_byte",
    "old_end_byte",
    "new_end_byte",
    "start_point",
    "old_end_point",
    "new_end_point",
    NULL,
  };

  int ok = PyArg_ParseTupleAndKeywords(
    args,
    kwargs,
    "III(II)(II)(II)",
    keywords,
    &start_byte,
    &old_end_byte,
    &new_end_byte,
    &start_row,
    &start_column,
    &old_end_row,
    &old_end_column,
    &new_end_row,
    &new_end_column
  );

  if (ok) {
    TSInputEdit edit = {
      .start_byte = start_byte,
      .old_end_byte = old_end_byte,
      .new_end_byte = new_end_byte,
      .start_point = {start_row, start_column},
      .old_end_point = {old_end_row, old_end_column},
      .new_end_point = {new_end_row, new_end_column},
    };
    ts_tree_edit(self->tree, &edit);
    self->edited = 1;
  }
  Py_RETURN_NONE;
}

static PyMethodDef tree_methods[] = {
  {
    .ml_name = "walk",
    .ml_meth = (PyCFunction)tree_walk,
    .ml_flags = METH_NOARGS,
    .ml_doc = "walk()\n--\n\n\
               Get a tree cursor for walking this tree.",
  },
  {
    .ml_name = "edit",
    .ml_meth = (PyCFunction)tree_edit,
    .ml_flags = METH_KEYWORDS|METH_VARARGS,
    .ml_doc = "edit(start_byte, old_end_byte, new_end_byte,\
               start_point, old_end_point, new_end_point)\n--\n\n\
               Edit the syntax tree.",
  },
  {NULL},
};

static PyGetSetDef tree_accessors[] = {
  {"root_node", (getter)tree_get_root_node, NULL, "The root node of this tree.", NULL},
  {"text", (getter)tree_get_text, NULL, "The source text for this tree, if unedited.", NULL},
  {NULL}
};

static PyTypeObject tree_type = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "tree_sitter.Tree",
  .tp_doc = "A Syntax Tree",
  .tp_basicsize = sizeof(Tree),
  .tp_itemsize = 0,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_dealloc = (destructor)tree_dealloc,
  .tp_methods = tree_methods,
  .tp_getset = tree_accessors,
};

static PyObject *tree_new_internal(TSTree *tree, PyObject *source) {
  Tree *self = (Tree *)tree_type.tp_alloc(&tree_type, 0);
  if (self != NULL) self->tree = tree;

  self->edited = 0;
  self->source = source;
  Py_INCREF(self->source);
  return (PyObject *)self;
}

// TreeCursor

static void tree_cursor_dealloc(TreeCursor *self) {
  ts_tree_cursor_delete(&self->cursor);
  Py_XDECREF(self->node);
  Py_XDECREF(self->tree);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *tree_cursor_get_node(TreeCursor *self, void *payload) {
  if (!self->node) {
    self->node = node_new_internal(ts_tree_cursor_current_node(&self->cursor), self->tree);
  }

  Py_INCREF(self->node);
  return self->node;
}

static PyObject *tree_cursor_current_field_name(TreeCursor *self, PyObject *args) {
  const char *field_name = ts_tree_cursor_current_field_name(&self->cursor);
  if (field_name == NULL) {
    Py_RETURN_NONE;
  }
  return PyUnicode_FromString(field_name);
}

static PyObject *tree_cursor_goto_parent(TreeCursor *self, PyObject *args) {
  bool result = ts_tree_cursor_goto_parent(&self->cursor);
  if (result) {
    Py_XDECREF(self->node);
    self->node = NULL;
  }
  return PyBool_FromLong(result);
}

static PyObject *tree_cursor_goto_first_child(TreeCursor *self, PyObject *args) {
  bool result = ts_tree_cursor_goto_first_child(&self->cursor);
  if (result) {
    Py_XDECREF(self->node);
    self->node = NULL;
  }
  return PyBool_FromLong(result);
}

static PyObject *tree_cursor_goto_next_sibling(TreeCursor *self, PyObject *args) {
  bool result = ts_tree_cursor_goto_next_sibling(&self->cursor);
  if (result) {
    Py_XDECREF(self->node);
    self->node = NULL;
  }
  return PyBool_FromLong(result);
}

static PyMethodDef tree_cursor_methods[] = {
  {
    .ml_name = "current_field_name",
    .ml_meth = (PyCFunction)tree_cursor_current_field_name,
    .ml_flags = METH_NOARGS,
    .ml_doc = "current_field_name()\n--\n\n\
               Get the field name of the tree cursor's current node.\n\n\
               If the current node has the field name, return str. Otherwise, return None.",
  },
  {
    .ml_name = "goto_parent",
    .ml_meth = (PyCFunction)tree_cursor_goto_parent,
    .ml_flags = METH_NOARGS,
    .ml_doc = "goto_parent()\n--\n\n\
               Go to parent.\n\n\
               If the current node is not the root, move to its parent and\n\
               return True. Otherwise, return False.",
  },
  {
    .ml_name = "goto_first_child",
    .ml_meth = (PyCFunction)tree_cursor_goto_first_child,
    .ml_flags = METH_NOARGS,
    .ml_doc = "goto_first_child()\n--\n\n\
               Go to first child.\n\n\
               If the current node has children, move to the first child and\n\
               return True. Otherwise, return False.",
  },
  {
    .ml_name = "goto_next_sibling",
    .ml_meth = (PyCFunction)tree_cursor_goto_next_sibling,
    .ml_flags = METH_NOARGS,
    .ml_doc = "goto_next_sibling()\n--\n\n\
               Go to next sibling.\n\n\
               If the current node has a next sibling, move to the next sibling\n\
               and return True. Otherwise, return False.",
  },
  {NULL},
};

static PyGetSetDef tree_cursor_accessors[] = {
  {"node", (getter)tree_cursor_get_node, NULL, "The current node.", NULL},
  {NULL},
};

static PyTypeObject tree_cursor_type = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "tree_sitter.TreeCursor",
  .tp_doc = "A syntax tree cursor.",
  .tp_basicsize = sizeof(TreeCursor),
  .tp_itemsize = 0,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_dealloc = (destructor)tree_cursor_dealloc,
  .tp_methods = tree_cursor_methods,
  .tp_getset = tree_cursor_accessors,
};

static PyObject *tree_cursor_new_internal(TSNode node, PyObject *tree) {
  TreeCursor *self = (TreeCursor *)tree_cursor_type.tp_alloc(&tree_cursor_type, 0);
  if (self != NULL) {
    self->cursor = ts_tree_cursor_new(node);
    Py_INCREF(tree);
    self->tree = tree;
  }
  return (PyObject *)self;
}

// Parser

static PyObject *parser_new(
  PyTypeObject *type,
  PyObject *args,
  PyObject *kwds
) {
  Parser *self = (Parser *)type->tp_alloc(type, 0);
  if (self != NULL) self->parser = ts_parser_new();
  return (PyObject *)self;
}

static void parser_dealloc(Parser *self) {
  ts_parser_delete(self->parser);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *parser_parse(Parser *self, PyObject *args) {
  PyObject *source_code = NULL;
  PyObject *old_tree_arg = NULL;
  if (!PyArg_UnpackTuple(args, "ref", 1, 2, &source_code, &old_tree_arg)) {
    return NULL;
  }

  if (!PyBytes_Check(source_code)) {
    PyErr_SetString(PyExc_TypeError, "First argument to parse must be bytes");
    return NULL;
  }

  const TSTree *old_tree = NULL;
  if (old_tree_arg) {
    if (!PyObject_IsInstance(old_tree_arg, (PyObject *)&tree_type)) {
      PyErr_SetString(PyExc_TypeError, "Second argument to parse must be a Tree");
      return NULL;
    }

    old_tree = ((Tree *)old_tree_arg)->tree;
  }

  size_t length = PyBytes_Size(source_code);
  char *source_bytes = PyBytes_AsString(source_code);
  TSTree *new_tree = ts_parser_parse_string(self->parser, old_tree, source_bytes, length);

  if (!new_tree) {
    PyErr_SetString(PyExc_ValueError, "Parsing failed");
    return NULL;
  }

  return tree_new_internal(new_tree, source_code);
}

static PyObject *parser_set_language(Parser *self, PyObject *arg) {
  PyObject *language_id = PyObject_GetAttrString(arg, "language_id");
  if (!language_id) {
    PyErr_SetString(PyExc_TypeError, "Argument to set_language must be a Language");
    return NULL;
  }

  if (!PyLong_Check(language_id)) {
    PyErr_SetString(PyExc_TypeError, "Language ID must be an integer");
    return NULL;
  }

  TSLanguage *language = (TSLanguage *)PyLong_AsVoidPtr(language_id);
  if (!language) {
    PyErr_SetString(PyExc_ValueError, "Language ID must not be null");
    return NULL;
  }

  unsigned version = ts_language_version(language);
  if (version < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION || TREE_SITTER_LANGUAGE_VERSION < version) {
    return PyErr_Format(
      PyExc_ValueError,
      "Incompatible Language version %u. Must not be between %u and %u",
      version,
      TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION,
      TREE_SITTER_LANGUAGE_VERSION
    );
  }

  ts_parser_set_language(self->parser, language);
  Py_RETURN_NONE;
}

static PyMethodDef parser_methods[] = {
  {
    .ml_name = "parse",
    .ml_meth = (PyCFunction)parser_parse,
    .ml_flags = METH_VARARGS,
    .ml_doc = "parse(bytes, old_tree=None)\n--\n\n\
               Parse source code, creating a syntax tree.",
  },
  {
    .ml_name = "set_language",
    .ml_meth = (PyCFunction)parser_set_language,
    .ml_flags = METH_O,
    .ml_doc = "set_language(language)\n--\n\n\
               Set the parser language.",
  },
  {NULL},
};

static PyTypeObject parser_type = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "tree_sitter.Parser",
  .tp_doc = "A Parser",
  .tp_basicsize = sizeof(Parser),
  .tp_itemsize = 0,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_new = parser_new,
  .tp_dealloc = (destructor)parser_dealloc,
  .tp_methods = parser_methods,
};

// Query

static PyObject *query_matches(Query *self, PyObject *args) {
  PyErr_SetString(PyExc_NotImplementedError, "Not Implemented");
  return NULL;
}

static PyObject *query_captures(Query *self, PyObject *args, PyObject *kwargs) {
  char *keywords[] = {
    "node",
    "start_point",
    "end_point",
    NULL,
  };

  Node *node = NULL;
  unsigned start_row = 0, start_column = 0, end_row = 0, end_column = 0;

  int ok = PyArg_ParseTupleAndKeywords(
    args,
    kwargs,
    "O|(II)(II)",
    keywords,
    (PyObject **)&node,
    &start_row,
    &start_column,
    &end_row,
    &end_column
  );
  if (!ok) return NULL;

  if (!PyObject_IsInstance((PyObject *)node, (PyObject *)&node_type)) {
    PyErr_SetString(PyExc_TypeError, "First argument to captures must be a Node");
    return NULL;
  }

  if (!query_cursor) query_cursor = ts_query_cursor_new();
  ts_query_cursor_exec(query_cursor, self->query, node->node);

  PyObject *result = PyList_New(0);

  uint32_t capture_index;
  TSQueryMatch match;
  while (ts_query_cursor_next_capture(query_cursor, &match, &capture_index)) {
    const TSQueryCapture *capture = &match.captures[capture_index];
    PyObject *capture_node = node_new_internal(capture->node, node->tree);
    PyObject *capture_name = PyList_GetItem(self->capture_names, capture->index);
    PyList_Append(result, PyTuple_Pack(2, capture_node, capture_name));
    Py_XDECREF(capture_node);
    Py_XDECREF(capture_name);
  }

  return result;
}

static void query_dealloc(Query *self) {
  if (self->query) ts_query_delete(self->query);
  Py_XDECREF(self->capture_names);
  Py_TYPE(self)->tp_free(self);
}

static PyMethodDef query_methods[] = {
  {
    .ml_name = "matches",
    .ml_meth = (PyCFunction)query_matches,
    .ml_flags = METH_VARARGS,
    .ml_doc = "matches(node)\n--\n\n\
               Get a list of all of the matches within the given node."
  },
  {
    .ml_name = "captures",
    .ml_meth = (PyCFunction)query_captures,
    .ml_flags = METH_KEYWORDS|METH_VARARGS,
    .ml_doc = "captures(node)\n--\n\n\
               Get a list of all of the captures within the given node.",
  },
  {NULL},
};

static PyTypeObject query_type = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "tree_sitter.Query",
  .tp_doc = "A set of patterns to search for in a syntax tree.",
  .tp_basicsize = sizeof(Query),
  .tp_itemsize = 0,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_dealloc = (destructor)query_dealloc,
  .tp_methods = query_methods,
};

static PyObject *query_new_internal(
  TSLanguage *language,
  char *source,
  int length
) {
  Query *query = (Query *)query_type.tp_alloc(&query_type, 0);
  if (query == NULL) return NULL;

  uint32_t error_offset;
  TSQueryError error_type;
  query->query = ts_query_new(
    language, source, length, &error_offset, &error_type
  );
  if (!query->query) {
    char *word_start = &source[error_offset];
    char *word_end = word_start;
    while (
      word_end < &source[length] &&
      (iswalnum(*word_end) || *word_end == '-' || *word_end == '_' || *word_end == '?' || *word_end == '.')
    ) word_end++;
    char c = *word_end;
    *word_end = 0;
    switch (error_type) {
      case TSQueryErrorNodeType:
        PyErr_Format(PyExc_NameError, "Invalid node type %s", &source[error_offset]);
        break;
      case TSQueryErrorField:
        PyErr_Format(PyExc_NameError, "Invalid field name %s", &source[error_offset]);
        break;
      case TSQueryErrorCapture:
        PyErr_Format(PyExc_NameError, "Invalid capture name %s", &source[error_offset]);
        break;
      default:
        PyErr_Format(PyExc_SyntaxError, "Invalid syntax at offset %u", error_offset);
    }
    *word_end = c;
    query_dealloc(query);
    return NULL;
  }

  unsigned n = ts_query_capture_count(query->query);
  query->capture_names = PyList_New(n);
  Py_INCREF(Py_None);
  for (unsigned i = 0; i < n; i++) {
    unsigned length;
    const char *capture_name = ts_query_capture_name_for_id(query->query, i, &length);
    PyList_SetItem(query->capture_names, i, PyUnicode_FromStringAndSize(capture_name, length));
  }
  return (PyObject *)query;
}

// Module

static PyObject *language_field_id_for_name(PyObject *self, PyObject *args) {
  TSLanguage *language;
  PyObject *language_id;
  char *field_name;
  Py_ssize_t length;
  if (!PyArg_ParseTuple(args, "Os#", &language_id, &field_name, &length)) {
    return NULL;
  }

  language = (TSLanguage *)PyLong_AsVoidPtr(language_id);

  TSFieldId field_id = ts_language_field_id_for_name(language, field_name, length);
  if (field_id == 0) {
    Py_RETURN_NONE;
  }

  return PyLong_FromSize_t((size_t)field_id);
}

static PyObject *language_query(PyObject *self, PyObject *args) {
  TSLanguage *language;
  PyObject *language_id;
  char *source;
  Py_ssize_t length;
  if (!PyArg_ParseTuple(args, "Os#", &language_id, &source, &length)) {
    return NULL;
  }

  language = (TSLanguage *)PyLong_AsVoidPtr(language_id);

  return query_new_internal(language, source, length);
}

static PyMethodDef module_methods[] = {
  {
    .ml_name = "_language_field_id_for_name",
    .ml_meth = (PyCFunction)language_field_id_for_name,
    .ml_flags = METH_VARARGS,
    .ml_doc = "(internal)",
  },
  {
    .ml_name = "_language_query",
    .ml_meth = (PyCFunction)language_query,
    .ml_flags = METH_VARARGS,
    .ml_doc = "(internal)",
  },
  {NULL},
};

static struct PyModuleDef module_definition = {
  .m_base = PyModuleDef_HEAD_INIT,
  .m_name = "binding",
  .m_doc = NULL,
  .m_size = -1,
  .m_methods = module_methods,
};

PyMODINIT_FUNC PyInit_binding(void) {
  PyObject *module = PyModule_Create(&module_definition);
  if (module == NULL) return NULL;

  if (PyType_Ready(&parser_type) < 0) return NULL;
  Py_INCREF(&parser_type);
  PyModule_AddObject(module, "Parser", (PyObject *)&parser_type);

  if (PyType_Ready(&tree_type) < 0) return NULL;
  Py_INCREF(&tree_type);
  PyModule_AddObject(module, "Tree", (PyObject *)&tree_type);

  if (PyType_Ready(&node_type) < 0) return NULL;
  Py_INCREF(&node_type);
  PyModule_AddObject(module, "Node", (PyObject *)&node_type);

  if (PyType_Ready(&tree_cursor_type) < 0) return NULL;
  Py_INCREF(&tree_cursor_type);
  PyModule_AddObject(module, "TreeCursor", (PyObject *)&tree_cursor_type);

  if (PyType_Ready(&query_type) < 0) return NULL;
  Py_INCREF(&query_type);
  PyModule_AddObject(module, "Query", (PyObject *)&query_type);

  return module;
}
