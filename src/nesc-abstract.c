/* This file is part of the nesC compiler.
   Copyright (C) 2002 Intel Corporation

The attached "nesC" software is provided to you under the terms and
conditions of the GNU General Public License Version 2 as published by the
Free Software Foundation.

nesC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with nesC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

#include "parser.h"
#include "nesc-cg.h"
#include "nesc-c.h"
#include "nesc-abstract.h"
#include "c-parse.h"
#include "nesc-component.h"
#include "nesc-semantics.h"
#include "nesc-configuration.h"
#include "AST_walk.h"
#include "semantics.h"
#include "constants.h"
#include "nesc-constants.h"
#include "c-lex.h"
#include "expr.h"
#include "nesc-env.h"

static AST_walker clone_walker;

/* data_declaration in:
   oldidentifier_decl: ignored as illegal in modules
   string

   enumerator
   function_decl
   identifier
   interface_deref
   typename
   variable_decl
   type_parm_decl
*/

/* tag_declaration in:
   tag_ref
   types
 */

/* field_declaration in:
   field_ref
   tag_declaration
 */

/* types in:
   (ignoring declared_type, currently unused)
   expression
   asttype
   field_declaration
   data_declaration
   (reptype in tag_declaration does not need instantiating)
 */

/* typelist in:
   data_declaration (gparms, can ignore oldstyle_args as not allowed in
                     modules)
*/

static void *clone(region r, void *vn)
{
  struct location l;
  node *n = vn;
  node new = AST_clone(r, *n);

  (*n)->instantiation = new;
  new->instantiation = NULL;
  *n = new;

  /* Update location to include the container (so we can print instance
     names in error messages) */
  l = *new->location;
  l.container = current.container;
  new->location = make_location(l);

  return new;
}

static void forward(data_declaration *dd)
{
  data_declaration ddecl = *dd;

  if (ddecl->instantiation)
    *dd = ddecl->instantiation;
}

static void instantiate_ddecl_types(data_declaration ddecl)
{
  ddecl->type = instantiate_type(ddecl->type);
  if (ddecl->kind == decl_interface_ref && ddecl->gparms)
    ddecl->gparms = instantiate_typelist(ddecl->gparms);
}

static void clone_ddecl(data_declaration ddecl)
{
  data_declaration copy;

  /* If already cloned, return. */
  if (ddecl->instantiation &&
      ddecl->instantiation->container == current.container)
    return;

  /* Copy module functions (incl. tasks) and variables */

  if (!(ddecl->kind == decl_variable || ddecl->kind == decl_function ||
	ddecl->kind == decl_constant || ddecl->kind == decl_typedef ||
	ddecl->kind == decl_interface_ref))
    return;

  /* Ignore non-module variables */
  if (!ddecl->container)
    return;

  copy = declare(current.env, ddecl, TRUE);
  ddecl->instantiation = copy;
  copy->fn_uses = NULL;
  copy->nuses = NULL;
  copy->shadowed = ddecl;
  copy->container = current.container;
  instantiate_ddecl_types(copy);
}

static void copy_fields(region r, tag_declaration copy, tag_declaration orig)
{
  field_declaration ofield, *nextfield;
  

  copy->fields = new_env(r, NULL);
  nextfield = &copy->fieldlist;

  for (ofield = orig->fieldlist; ofield; ofield = ofield->next)
    {
      field_declaration cfield = ralloc(r, struct field_declaration);

      *cfield = *ofield;
      cfield->type = instantiate_type(cfield->type);
      cfield->ast = CAST(field_decl, ofield->ast->instantiation);
      ofield->instantiation = cfield;
      cfield->instantiation = NULL;
      if (cfield->name)
	env_add(copy->fields, cfield->name, cfield);
      *nextfield = cfield;
      nextfield = &cfield->next;
    }
}

static void forward_tdecl(region r, tag_ref tref)
{
  tag_declaration tdecl = tref->tdecl, copy;  

  /* Ignore non-module tags */
  if (!tdecl->container)
    return;

  /* If already cloned, return */
  if (tdecl->instantiation &&
      tdecl->instantiation->container == current.container)
    return;

  copy = declare_tag(tref);
  tref->tdecl = copy;
  tdecl->instantiation = copy;

  copy->reptype = tdecl->reptype;
  if (tdecl->defined)
    copy_fields(r, copy, tdecl);
  copy->shadowed = tdecl;
  copy->defined = tdecl->defined;
  copy->fields_const = tdecl->fields_const;
  copy->fields_volatile = tdecl->fields_volatile;
  copy->transparent_union = tdecl->transparent_union;
  copy->collapsed = tdecl->collapsed;
  copy->container = current.container;
}

static AST_walker_result clone_expression(AST_walker spec, void *data,
					  expression *n)
{
  expression new = clone(data, n);

  /* A few nodes (related to initialisation) don't have types */
  if (new->type)
    new->type = instantiate_type(new->type);

  return aw_walk;
}

static AST_walker_result clone_asttype(AST_walker spec, void *data, asttype *n)
{
  asttype new = clone(data, n);

  new->type = instantiate_type(new->type);

  return aw_walk;
}

static AST_walker_result clone_function_decl(AST_walker spec, void *data,
					     function_decl *n)
{
  declaration old = CAST(declaration, *n);
  function_decl new = clone(data, n);

  clone_ddecl(new->ddecl);

  if (new->ddecl->instantiation)
    {
      data_declaration instance = new->ddecl->instantiation;

      /* We need to forward the ddecl *and* update the definition field in
	 the instantiated data_declaration. */
      instance->definition = CAST(declaration, new);
      /* We update the ast field if it pointed to this function_decl
	 (note that command and event data_declarations assume that the
	 ast field points to the original variable_decl) */
      if (instance->ast == old)
	instance->ast = CAST(declaration, new);
      new->ddecl = instance;
    }

  return aw_walk;
}

static AST_walker_result clone_identifier(AST_walker spec, void *data,
					  identifier *n)
{
  identifier new = clone(data, n);

  new->type = instantiate_type(new->type);
  forward(&new->ddecl);

  return aw_walk;
}

static AST_walker_result clone_interface_deref(AST_walker spec, void *data,
					     interface_deref *n)
{
  interface_deref new = clone(data, n);

  new->type = instantiate_type(new->type);
  forward(&new->ddecl);

  return aw_walk;
}

static AST_walker_result clone_variable_decl(AST_walker spec, void *data,
					     variable_decl *n)
{
  declaration old = CAST(declaration, *n);
  variable_decl new = clone(data, n);

  clone_ddecl(new->ddecl);

  if (new->ddecl->instantiation)
    {
      data_declaration instance = new->ddecl->instantiation;

      /* Forward the ddecl and update the ast and definition fields */
      if (instance->definition == old)
	instance->definition = CAST(declaration, new);
      if (instance->ast == old)
	instance->ast = CAST(declaration, new);
      new->ddecl = instance;
    }

  return aw_walk;
}

static AST_walker_result clone_type_parm_decl(AST_walker spec, void *data,
					     type_parm_decl *n)
{
  type_parm_decl new = clone(data, n);
  data_declaration instance;

  clone_ddecl(new->ddecl);
  instance = new->ddecl->instantiation;
  instance->definition = CAST(declaration, new);
  instance->ast = CAST(declaration, new);
  new->ddecl = instance;

  return aw_walk;
}

static AST_walker_result clone_typename(AST_walker spec, void *data,
					typename *n)
{
  typename new = clone(data, n);

  forward(&new->ddecl);

  return aw_walk;
}

static AST_walker_result clone_enumerator(AST_walker spec, void *data,
					  enumerator *n)
{
  enumerator new = clone(data, n);

  clone_ddecl(new->ddecl);

  if (new->ddecl->instantiation)
    {
      data_declaration instance = new->ddecl->instantiation;

      /* Forward the ddecl and update the ast and definition fields */
      instance->definition = CAST(declaration, new);
      instance->ast = CAST(declaration, new);
      new->ddecl = instance;
    }

  return aw_walk;
}

static AST_walker_result clone_tag_ref(AST_walker spec, void *data,
				       tag_ref *n)
{
  tag_ref new = clone(data, n);

  AST_walk_children(spec, data, CAST(node, new));
  forward_tdecl(data, new);
  if (new->defined)
    new->tdecl->definition = new;

  return aw_done;
}

static AST_walker_result clone_field_ref(AST_walker spec, void *data,
					 field_ref *n)
{
  field_ref new = clone(data, n);

  new->type = instantiate_type(new->type);
  if (new->fdecl->instantiation)
    new->fdecl = new->fdecl->instantiation;

  return aw_walk;
}

static void set_ddecl_instantiation1(data_declaration fndecl, void *data)
{
  data_declaration orig = fndecl;

  instantiate_ddecl_types(fndecl);

  /* Here we make the copy of the fndecl created during parsing
     (the copy from the actual interface type) point to fndecl.
     Note that the last shadowed pointer points to the entries in
     the interface type */
  while (orig->shadowed->shadowed)
    orig = orig->shadowed;

  assert(orig != fndecl);
  orig->instantiation = fndecl;
}

static void set_specification_instantiations(nesc_declaration component)
/* Effects: Set the instantiation pointers in the data_declarations of
     the original abstract component from which component is derived to
     the copies in component (in preparation for cloning component's
     AST and pointing to component's decls)

     Also instantiate the types in the copies

     The original data_declarations can be found by following the
     shadowed fields. We may have to follow these one deep (abstract
     modules in configurations) or two deep (abstract modules in
     abstract configurations)...
*/
{
  component_functions_iterate(component, set_ddecl_instantiation1, NULL);
}

static void set_ddecl_instantiation2(data_declaration fndecl, void *data)
{
  /* We just make the decl fndecl is a copy of point back to fndecl */
  fndecl->shadowed->instantiation = fndecl;
}

static void set_specification_instantiations_shallow(nesc_declaration component)
/* Effects: Set the instantiation pointers in the data_declarations of
     the original abstract component from which component is derived to
     the copies in component (in preparation for cloning component's
     AST and pointing to component's decls)

     The original data_declarations can be found by following the
     shadowed fields. We may have to follow these one deep (abstract
     modules in configurations) or two deep (abstract modules in
     abstract configurations)...
*/
{
  component_functions_iterate(component, set_ddecl_instantiation2, NULL);
}

static declaration instantiate_parameters(region r, declaration orig_parms)
/* Effects: Makes a new list of declarations for an abstract componnent
*/
{
  /* A new dummy env for the instantiated parameters */
  current.env = new_environment(r, NULL, TRUE, FALSE);
  AST_walk_list(clone_walker, r, CASTPTR(node, &orig_parms));
  AST_set_parents(CAST(node, orig_parms));

  return CAST(declaration, orig_parms);
}

static void instantiate_endp(endp ep)
/* Effects: Modifies ep based on instantiated ddecls
 */
{
  /* The component does not get instantiated and is ignored anyway */
  if (ep->interface->instantiation)
    ep->interface = ep->interface->instantiation;
  if (ep->function->instantiation)
    ep->function = ep->function->instantiation;
  if (ep->args_node)
    ep->args_node = CAST(parameterised_identifier,
			 ep->args_node->instantiation);
}

static void instantiate_cg(cgraph copy, cgraph original)
/* Effects: Copies the original graph into copy, with endpoints based
     on the instantiations specified in the function and interface ddecls
*/
{
  ggraph orig_g = cgraph_graph(original);
  gnode n;
  gedge connection;

  /* Add all edges from original to copy, but with updated ddecls */
  graph_scan_nodes (n, orig_g)
    {
      struct endp from = *NODE_GET(endp, n);
      gnode cfrom;

      instantiate_endp(&from);
      cfrom = endpoint_lookup(copy, &from);

      graph_scan_out (connection, n)
	{
	  struct endp to = *NODE_GET(endp, graph_edge_to(connection));
	  gnode cto;

	  instantiate_endp(&to);
	  cto = endpoint_lookup(copy, &to);

	  graph_add_edge(cfrom, cto, NULL);
	}
    }
}

static AST_walker_result clone_component_ref(AST_walker spec, void *data,
					     component_ref *n)
{
  component_ref new = clone(data, n);

  /* Instantiate any further abstract components inside this abstract
     configuration. */
  if (new->cdecl->abstract)
    {
      new->cdecl = specification_copy(data, new, FALSE);
      set_specification_instantiations_shallow(new->cdecl);
    }

  return aw_walk;
}

static AST_walker_result clone_interface_ref(AST_walker spec, void *data,
					     interface_ref *n)
{
  interface_ref new = clone(data, n);

  AST_walk_children(spec, data, CAST(node, new));

  clone_ddecl(new->ddecl);
  new->ddecl = new->ddecl->instantiation;

  if (new->ddecl->itype->abstract)
    new->ddecl->itype = interface_copy(data, new, current.container->abstract);

  copy_interface_functions(data, current.container, 
			   new->ddecl, new->ddecl->functions);

  return aw_done;
}

static AST_walker_result clone_configuration(AST_walker spec, void *data,
					     configuration *n)
{
  configuration new = clone(data, n);
  nesc_declaration comp = current.container;

  /* Copy the components and connections */
  AST_walk_children(spec, data, CAST(node, new));

  /* Copy the connection graph
     (note that comp->connections was initialised to an "empty" graph */
  instantiate_cg(comp->connections, original_component(comp)->connections);

  return aw_done;
}

static AST_walker_result clone_ast(AST_walker spec, void *data, node *n)
{
  clone(data, n);
  return aw_walk;
}

static void init_clone(void)
{
  clone_walker = new_AST_walker(permanent);
  AST_walker_handle(clone_walker, kind_node, clone_ast);

  AST_walker_handle(clone_walker, kind_expression, clone_expression);
  AST_walker_handle(clone_walker, kind_field_ref, clone_field_ref);
  AST_walker_handle(clone_walker, kind_identifier, clone_identifier);
  AST_walker_handle(clone_walker, kind_interface_deref, clone_interface_deref);

  AST_walker_handle(clone_walker, kind_asttype, clone_asttype);
  AST_walker_handle(clone_walker, kind_function_decl, clone_function_decl);
  AST_walker_handle(clone_walker, kind_variable_decl, clone_variable_decl);
  AST_walker_handle(clone_walker, kind_type_parm_decl, clone_type_parm_decl);
  AST_walker_handle(clone_walker, kind_typename, clone_typename);
  AST_walker_handle(clone_walker, kind_enumerator, clone_enumerator);
  AST_walker_handle(clone_walker, kind_configuration, clone_configuration);
  AST_walker_handle(clone_walker, kind_component_ref, clone_component_ref);
  AST_walker_handle(clone_walker, kind_interface_ref, clone_interface_ref);
  AST_walker_handle(clone_walker, kind_tag_ref, clone_tag_ref);
}

void set_parameter_values(nesc_declaration cdecl, expression args)
{
  declaration parm;

  /* We know args is the same length as parameters (earlier error if not) */
  scan_declaration (parm, cdecl->parameters)
    {
      if (is_data_decl(parm))
	{
	  variable_decl vd = CAST(variable_decl, CAST(data_decl, parm)->decls);

	  if (!is_type_argument(args) && check_constant_once(args))
	    {
	      location l = args->location;

	      /* We can assume the type is arithmetic (for now at least)
		 (see declare_template_parameter) */
	      if (!args->cst)
		error_with_location(l, "component arguments must be constants");
	      else if (type_integer(vd->ddecl->type))
		{
		  if (!constant_integral(args->cst))
		    error_with_location(l, "integer constant expected");
		  else if (!cval_inrange(args->cst->cval, vd->ddecl->type))
		    error_with_location(l, "constant out of range for argument type");
		}
	      else if (type_floating(vd->ddecl->type))
		{
		  if (!constant_float(args->cst))
		    error_with_location(l, "floating-point constant expected");
		}
	      else
		assert(0);
	    }

	  vd->ddecl->value = args->cst;
	}
      else /* type */
	{
	  type_parm_decl td = CAST(type_parm_decl, parm);

	  td->ddecl->type = args->type;
	  td->ddecl->initialiser = args;
	}

      args = CAST(expression, args->next);
    }
}

static void set_parameter_instantiations(nesc_declaration cdecl)
{
  declaration parm;

  /* We know args is the same length as parameters (earlier error if not) */
  scan_declaration (parm, cdecl->parameters)
    {
      data_declaration ddecl, orig;

      if (is_data_decl(parm))
	ddecl = CAST(variable_decl, CAST(data_decl, parm)->decls)->ddecl;
      else
	ddecl = CAST(type_parm_decl, parm)->ddecl;

      orig = original_declaration(ddecl);
      assert(ddecl != orig);
      orig->instantiation = ddecl;
    }
}

node instantiate_ast_list(region r, node n)
{
  AST_walk_list(clone_walker, r, &n);
  AST_set_parents(n);

  return n;
}

void instantiate(nesc_declaration component, expression arglist)
/* Effects: Actually instantiate an abstract component
     For modules: temp noop
     For configurations: make new shallow copies of included abstract
       components, and copy connection graph (using the new shallow
       copies) 
*/
{
  region r = parse_region;

  assert(component->kind == l_component && component->original);
  current.container = component;

  /* We don't copy the component itself as we're handling the specification
     specially (not copied). So we just copy the parameters and the
     implementation. */

#if 0
  component->parameters = instantiate_parameters(r, component->parameters);
  set_parameter_values(component, arglist);
#endif
  set_parameter_instantiations(component);
  set_specification_instantiations(component);

  /* A new dummy env for all instantiations in the implementation */
  current.env = new_environment(r, NULL, TRUE, FALSE);
  component->impl = CAST(implementation,
			 instantiate_ast_list(r, CAST(node, component->impl)));
}

/* Component stack handling, for error message and loop detection */

struct instance_stack
{
  struct instance_stack *next;
  nesc_declaration component;
};

static struct instance_stack *stack, *avail;

static struct instance_stack *new_instance_stack(void)
/* Returns: a new, cleared, instance_stack
 */
{
  struct instance_stack *new;

  if (avail)
    {
      new = avail;
      avail = avail->next;
      new->next = NULL;
    }
  else
    new = ralloc(permanent, struct instance_stack);

  return new;
}

static void free_instance_stack(struct instance_stack *is)
{
  is->next = avail;
  is->component = NULL;
  avail = is;
}

void push_instance(nesc_declaration component)
/* Effects: push (concrete) component on the stack and set its full instance
     name.
*/
{
  struct instance_stack *new = new_instance_stack();

  assert(!component->abstract);
  if (component->original)
    {
      /* Instantiated component names is parent name (currently at the top
	 of the stack) . name-in-configuration (currently in instance_name) */
      const char *oldname = component->instance_name;
      const char *parentname = stack->component->instance_name;
      int namelen = strlen(parentname) + strlen(oldname) + 2;
      char *newname;

      newname = rstralloc(parse_region, namelen);
      sprintf(newname, "%s.%s", parentname, oldname);
      component->instance_name = newname;
    }

  new->next = stack;
  stack = new;
  new->component = component;

  current.container = component;
}

nesc_declaration abstract_recursion(void)
/* Returns:  If the instance stack indicates the programmer has
     created an instantiation loop, i.e., component Y (instance of
     abstract component X) has caused the instantiation of the top-most
     component (another instance of X).
     Return Y if this is the case, NULL if not.
*/
{
  struct instance_stack *i;
  nesc_declaration component = stack->component;

  /* The case where component is not an instance falls through
     naturally */
  component = original_component(component);

  for (i = stack->next; i; i = i->next)
    {
      /* If we hit a non-instance component there isn't a loop */
      if (!i->component->original)
	return NULL;

      if (original_component(i->component) == component)
	return i->component;
    }
  return NULL;
}

void pop_instance(void)
{
  struct instance_stack *top = stack;

  stack = stack->next;
  free_instance_stack(top);

  if (stack)
    current.container = stack->component;
  else
    current.container = NULL;
}

static void check_cg(cgraph connections)
/* Effects: Checks constants used in the connections graph
 */
{
  ggraph g = cgraph_graph(connections);
  gnode n;

  graph_scan_nodes (n, g)
    {
      endp ep = NODE_GET(endp, n);

      if (ep->args_node)
	check_generic_arguments(ep->args_node->args, endpoint_args(ep));
    }
}

static bool fold_components(nesc_declaration cdecl, int pass)
{
  bool done;

  if (cdecl->folded == pass)
    return TRUE;

  cdecl->folded = pass;

  done = fold_constants_list(CAST(node, cdecl->impl), pass);

  if (is_module(cdecl->impl))
    ;
  else
    {
      component_ref comp;
      configuration c = CAST(configuration, cdecl->impl);

      check_cg(cdecl->connections);

      scan_component_ref (comp, c->components)
	{
	  set_parameter_values(comp->cdecl, comp->args);
	  done = fold_components(comp->cdecl, pass) && done;
	}
    }
  return done;
}

void fold_program(nesc_declaration program)
{
  int pass = 1;
  bool done;

  do
    {
      done = fold_constants_list(CAST(node, all_cdecls), pass);
      if (program)
	done = fold_components(program, pass) && done;
      pass++;
    }
  while (!done);

  current.container = NULL;
}

bool check_abstract_arguments(const char *kind, data_declaration ddecl,
			      declaration parms, expression arglist)
{
  location loc = ddecl->ast->location;
  int parmnum = 1;
  int oldcount = errorcount;

  while (parms && arglist)
    {
      if (is_data_decl(parms))
	{
	  variable_decl vparm = CAST(variable_decl, CAST(data_decl, parms)->decls);
	  type parmtype = vparm->ddecl->type;

	  if (type_incomplete(parmtype))
	    error_with_location(loc, "type of formal parameter %d is incomplete", parmnum);
	  else if (is_type_argument(arglist))
	    error_with_location(loc, "formal parameter %d must be a value", parmnum);
	  else 
	    {
	      set_error_location(arglist->location);
	      check_assignment(parmtype, default_conversion_for_assignment(arglist),
			       arglist, NULL, ddecl, parmnum);
	    }
	}
      else /* type argument */
	{
	  if (!is_type_argument(arglist))
	    error_with_location(loc, "formal parameter %d must be a type", parmnum);
	  else if (type_array(arglist->type))
	    error_with_location(loc, "type parameter cannot be an array type (parameter %d)",
		  parmnum);
	  else if (type_function(arglist->type))
	    error_with_location(loc, "type parameter cannot be a function type (parameter %d)",
		  parmnum);
	  else if (type_incomplete(arglist->type))
	    error_with_location(loc, "type parameter %d is an incomplete type", parmnum);
	}
      parmnum++;
      arglist = CAST(expression, arglist->next);
      parms = CAST(declaration, parms->next);
    }
  clear_error_location();

  if (parms)
    error_with_location(loc, "too few arguments to %s `%s'",
			kind, ddecl->name);
  else if (arglist)
    error_with_location(loc, "too many arguments to component `%s'",
			kind, ddecl->name);

  return oldcount == errorcount;
}

static nesc_declaration 
nesc_declaration_copy(region r, nesc_declaration old, expression args,
		      bool copy_is_abstract)
{
  nesc_declaration copy;

  copy = new_nesc_declaration(r, old->kind, old->name);
  copy->env = new_environment(r, old->env->parent, TRUE, FALSE);
  copy->short_docstring = old->short_docstring;
  copy->long_docstring = old->long_docstring;
  copy->abstract = copy_is_abstract;
  copy->original = old;

  /* Copy the parameters, storing the new decls in a dummy env */
  current.env = new_environment(r, NULL, TRUE, FALSE);
  current.container = copy;
  copy->parameters = instantiate_parameters(r, old->parameters);
  set_parameter_values(copy, args);

  return copy;
}

nesc_declaration interface_copy(region r, interface_ref iref,
				bool copy_is_abstract)
/* Returns: A copy of abstract interface intf, instantiated with arguments
     in arglist.
*/
{
  nesc_declaration intf = iref->ddecl->itype, copy;
  struct semantic_state old = current;

  assert(intf->kind == l_interface);

  copy = nesc_declaration_copy(r, intf, iref->args, copy_is_abstract);
  current.env = copy->env;
  copy->ast = CAST(nesc_decl, instantiate_ast_list(r, CAST(node, intf->ast)));
  current = old;
  
  return copy;
}

nesc_declaration specification_copy(region r, component_ref cref,
				    bool copy_is_abstract)
/* Returns: A copy of the parameters and specification of the
     component specified by cref, with arguments specified by cref
*/
{
  component spec;
  nesc_declaration comp = cref->cdecl, copy;
  struct semantic_state old = current;

  assert(comp->kind == l_component);

  copy = nesc_declaration_copy(r, comp, cref->args, copy_is_abstract);
  copy->instance_name = cref->word2->cstring.data;
  if (!copy_is_abstract)
    {
      /* Give it a new name */
      /* component may itself be a copy of the real original abstract
	 component */
      nesc_declaration abs_comp = comp->original ? comp->original : comp;
      char *newname = rstralloc(r, strlen(copy->name) + 20);

      sprintf(newname, "%s$%d", copy->name, abs_comp->instance_count++);
      copy->name = newname;
    }

  copy->ast = comp->ast;
  clone(r, &copy->ast);
  copy->impl = comp->impl;

  /* Copy the specification into the copy's env */
  current.env = copy->env;
  spec = CAST(component, copy->ast);
  spec->rplist = CAST(rp_interface,
		      instantiate_ast_list(r, CAST(node, spec->rplist)));
  current = old;

  /* Give the copy an "empty" specification graph */
  copy->connections = build_external_graph(r, copy);

  return copy;
}

void init_abstract(void)
{
  init_clone();
}
