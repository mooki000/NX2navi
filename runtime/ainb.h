// AINB logic graphs (ainb.c): the game's behaviour files, one per enemy, character and room (AI/, Logic/ and Sequence/
// graphs, loose or members of actor packs), version 0x408, little-endian. restore_engine.c changes their structure with
// it: find a node, set a value, add a copy of a node, connect an output to an input.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Ainb Ainb;

// The graph p[0..n) read (the bytes are copied); NULL with why when it is not one this reader can write back.
Ainb *ainb_read(const uint8_t *p, size_t n, const char **why);
void ainb_free(Ainb *g);
// The graph as a file (malloc'd): its sections in the order the game's files have them, each where the one before it
// ends, every offset into the file moved with what it points to. A graph read and not edited comes out as it was.
uint8_t *ainb_write(const Ainb *g, size_t *out_n);
// The number of nodes, and node i's type ("QueryMathAddS32", "Simultaneous").
int ainb_nodes(const Ainb *g);
const char *ainb_type(const Ainb *g, int i);
// A name for node i that the edits after the one that gave it use ("@name"); 0 when there is no room for it.
int ainb_name(Ainb *g, const char *name, int i);
// The node named so; -1 when none is.
int ainb_named(const Ainb *g, const char *name);

// The edits. Each writes a sentence into why[0..why_n) saying what it did or why it did nothing.
//
// The one node of type `type` whose values match `match`, items separated by ';': "Key=value" (a property, or an
// input of its own, holding that value), "Key<-@name.Output" or "Key<-#3.Output" (an input connected to that output of
// the node named so, or of node 3), "Key<-bb:Name" (bound to the blackboard entry Name), "#3" (node 3 itself). Its
// index, else -1: no node of that type, none matching, or more than one.
int ainb_find(const Ainb *g, const char *type, const char *match, char *why, size_t why_n);
// Property or input `param` of node i (holding a value of its own, not connected or bound) from `was` to `now`, both
// as text: a number, true or false, the string itself, or x,y,z. 1 when set; 2 when it holds now already; 0 when it
// holds something else, or there is no such parameter.
int ainb_set(Ainb *g, int i, const char *param, const char *was, const char *now, char *why, size_t why_n);
// A new node at the end, a copy of node t of the graph src (src may be g): its type, flags and values, its inputs
// holding the values they hold and connected to nothing, its outputs used by nothing. Its index, else -1: a node that
// has children, calls a module, holds an expression or attachments, or (from another graph) uses that graph's
// blackboard is not copied.
int ainb_add(Ainb *g, const Ainb *src, int t, char *why, size_t why_n);
// Output `out` of node `from` connected to input `in` of node `to`, which held `was`: "@name.Output" or "#3.Output" a
// connection, "bb:Name" a blackboard entry, "=value" a value of its own. The input's old connection goes, the queries
// node `to` asks before it runs follow. With was "+#3.Output" (or "+#3.Out,#5.Out" for a multi-input) the output is
// added to those the input reads (a multi-input, as OperateAND's BoolMulti). 1 when connected; 2 when connected so
// already; 0 when the input held something else or a name is wrong.
int ainb_link(Ainb *g, int from, const char *out, int to, const char *in, const char *was, char *why, size_t why_n);
