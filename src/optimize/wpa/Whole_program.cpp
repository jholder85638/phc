/*
 * phc -- the open source PHP compiler
 * See doc/license/README.license for licensing information
 *
 * Perform a number of whole-program analyses simulteneously.
 *
 * Design of the whole-program optimizer
 *
 *		1. Perform flow-sensitive, context-sensitive, object-sensitive,
 *		field-sensitive analysis. When analysing a function in multiple contexts,
 *		clone the function, and store the clones in the call-graph. 
 *
 *		2. The analyses provide feedback to the Whole-program analyser. They
 *		resolve types to reduce reduce conservatism at call sites and for call
 *		handlers, and help resolve branches.
 *
 *		3. After the analysis is complete, each function will have one set of
 *		results at each program-point, for each context. These contexts are then
 *		merged.
 *
 *		4. Once merged, the combined alias-solution is used to annotate the
 *		results for SSA, and local optimizations are run on each function
 *
 *		5. Once merged, a transformer is run over each function, bottom-up,
 *		transforming the graph.
 *
 *		6. This whole process iterates until it converges (or a fixed number of
 *		times). This allows evals and includes to be replaced
 *		with their respective code.
 *
 *		7. An optimization annotator then runs across the entire solution,
 *		annotating the MIR using results from relevant program points.
 *
 *		8. Finally, code is generated using the (hopefully) well-annotated
 *		code.
 */


/*
 * Try to list areas in which we must be conservative:
 *
 * include_*
 * require_*
 * eval
 * per-object properties for non-stdClasses
 *
 * Limited conservativeness
 * dl
 * extract
 * compact
 *
 *
 * Areas which we dont support that might take some work (ie exceptions)
 *
 * set_error_handler
 * set_exception_handler
 *
 *
 * Hidden effects:
 *
 * array_indexing for SPL::ArrayAccess
 * handlers of objects of unknown classes
 * __autoload
 *
 
 */

#include "process_ir/General.h"
#include "pass_manager/Pass_manager.h"

#include "optimize/Abstract_value.h"
#include "optimize/Edge.h"
#include "optimize/Method_pruner.h"
#include "optimize/Oracle.h"
#include "optimize/SCCP.h"

#include "Whole_program.h"
#include "WPA.h"

#include "Aliasing.h"
#include "Callgraph.h"
#include "CCP.h"
#include "Constant_state.h"
#include "Debug_WPA.h"
#include "Def_use.h"
#include "Include_analysis.h"
#include "Optimization_annotator.h"
#include "Optimization_transformer.h"
#include "Points_to.h"
#include "Type_inference.h"
#include "VRP.h"

using namespace MIR;
using namespace boost;
using namespace std;

Whole_program::Whole_program (Pass_manager* pm)
: pm (pm)
{
	annotator = new Optimization_annotator (this);
	transformer = new Optimization_transformer (this);
}

void
Whole_program::run (MIR::PHP_script* in)
{
	for (int w = 0; w < 10; w++)
	{
		initialize ();

		// Perform the whole-program analysis
		invoke_method (
				new Method_invocation (
					NULL,
					new METHOD_NAME (s("__MAIN__")),
					new Actual_parameter_list),
				NULL,
				NULL);


		// Optimize based on analysis results
		foreach (String* method, *callgraph->bottom_up ())
		{
			User_method_info* info = Oracle::get_user_method_info (method);

			if (info == NULL)
				continue;

			// Merge different contexts
			merge_contexts (info);

			// Apply the results
			apply_results (info);

			// Summarize the current results
			generate_summary (info);

			// These should converge fairly rapidly, I think
			for (int i = 0; i < 10; i++)
			{
				DEBUG ((i+1) << "th intraprocedural iteration for "
						<< *info->name);

				CFG* before = info->cfg->clone ();

				// Perform DCE and CP, and some small but useful optimizations.
				perform_local_optimizations (info);

				// Inlining and such.
				perform_interprocedural_optimizations (info);

				// Summarize the current results
				generate_summary (info);

				// Check if we can stop iterating.
				if (before->equals (info->cfg))
					break;
			}
		}

		// Check if we can stop iterating the Whole-program solution.
		DEBUG ((w+1) << "th Whole-program pass");
		if (analyses_have_converged ())
			break;

		if (w == 9)
			phc_TODO (); // on the examples I'm running, this shouldnt happen.
	}

	// All the analysis and iteration is done
	foreach (String* method, *callgraph->bottom_up ())
	{
		Method_info* method_info = Oracle::get_method_info (method);

		if (!method_info->has_implementation ())
			continue;

		User_method_info* info = dyc<User_method_info> (method_info);

		// Annotate the statements for code-generation
		annotate_results (info);

		// Replace method implementation with optimized code
		info->method->statements = info->cfg->get_linear_statements ();
	}

	// As a final step, strip all unused functions.	
	strip (in);
}

bool
Whole_program::analyses_have_converged ()
{
	if (old_analyses.size () == 0)
		return false;
	
	List<WPA*>::const_iterator i = old_analyses.begin();
	foreach_wpa (this)
	{
		if (!wpa->equals (*i))
		{
			DEBUG (wpa->name << " has not converged");
			return false;
		}

		i++;
	}

	return true;
}

void
Whole_program::initialize ()
{
	// save the old analyses for iteration
	old_analyses.clear ();
	old_analyses.push_back_all (&analyses);
	analyses.clear ();


	// Create new analyses with empty results
	aliasing = new Aliasing (this);
	callgraph = new Callgraph (this);
	ccp = new CCP (this);
	def_use = new Def_use (this);
	type_inf = new Type_inference (this);
//	constant_state = new Constant_state (this);
//	include_analysis = new Include_analysis (this);
//	vrp = new VRP (this);

	register_analysis ("debug-wpa", new Debug_WPA (this));
	register_analysis ("aliasing", aliasing);
	register_analysis ("callgraph", callgraph);
	register_analysis ("ccp", ccp);
	register_analysis ("def-use", def_use);
	register_analysis ("type-inference", type_inf);
//	register_analysis ("Constant_state", constant_state);
//	register_analysis ("Include_analysis", include_analysis);
//	register_analysis ("VRP", vrp);
}


void
Whole_program::analyse_function (User_method_info* info, Basic_block* caller, MIR::Actual_parameter_list* actuals, MIR::VARIABLE_NAME* lhs)
{
	CFG* cfg = info->cfg;

	// This is very similar to run() from Sparse_conditional_visitor, except
	// that it isnt sparse.

	if (debugging_enabled)
		cfg->dump_graphviz (s("Function entry"));

	// 1. Initialize:
	Edge_list* cfg_wl = new Edge_list (cfg->get_entry_edge ());

	foreach (Edge* e, *cfg->get_all_edges ())
		e->is_executable = false;

	// Process the entry blocks first (there is no edge here)
	DEBUG ("Initing functions");
	forward_bind (info, caller, cfg->get_entry_bb (), actuals);

	
	// 2. Stop when CFG-worklist is empty
	while (cfg_wl->size () > 0)
	{
		Edge* e = cfg_wl->front();
		cfg_wl->pop_front ();


		// Analyse the block, storing per-basic-block results.
		// This does not update the block structure.

		bool changed = false;
		
		// Always pass through at least once.
		if (e->is_executable == false)
			changed = true;

		// Tell successors that we are executable (do this before the target is
		// analysed).
		e->is_executable = true;

		changed |= analyse_block (e->get_target ());


		// Add next	block(s)
		if (changed)
		{
			if (Branch_block* branch = dynamic_cast<Branch_block*> (e->get_target ()))
				cfg_wl->push_back_all (get_branch_successors (branch));
			else if (!isa<Exit_block> (e->get_target ()))
				cfg_wl->push_back (e->get_target ()->get_successor_edges ()->front ());
		}
	}

	backward_bind (info, caller, cfg->get_exit_bb (), lhs);
}

Edge_list*
Whole_program::get_branch_successors (Branch_block* bb)
{
	Edge_list* result = new Edge_list;

	Alias_name cond = VN (ST (bb), bb->branch->variable_name)->name ();

	if (!ccp->branch_known_true (bb, cond))
		result->push_back (bb->get_false_successor_edge ());

	if (!ccp->branch_known_false (bb, cond))
		result->push_back (bb->get_true_successor_edge ());

	return result;
}

void
Whole_program::register_analysis (string name, WPA* analysis)
{
	analyses.push_back (analysis);
	analysis->name = name;
}

Method_info_list*
Whole_program::get_possible_receivers (Method_invocation* in)
{
	Method_info_list* result = new Method_info_list;

	// If there is a target or a variable_method, there may be > 1 methods that
	// match it.
	if (in->target)
		phc_TODO ();

	if (isa<Variable_method> (in->method_name))
		phc_TODO ();

	String* name = dyc<METHOD_NAME> (in->method_name)->value;

	// This assumes there is only 1 function of that name, which is true. If
	// there are multiple versions, they are lowered to different names before
	// MIR.
	Method_info* info = Oracle::get_method_info (name);
	if (info == NULL)
		phc_TODO ();

	result->push_back (info);

	return result;	
}

void
Whole_program::invoke_method (Method_invocation* in, Basic_block* context, MIR::VARIABLE_NAME* lhs)
{
	Method_info_list* receivers = get_possible_receivers (in);

	// Need to clone the information and merge it when it returns.
	if (receivers->size () > 1)
		phc_TODO ();

	
	foreach (Method_info* receiver, *receivers)
	{
		// TODO: where should I clone the actuals?
		analyse_method_info (receiver, context, in->actual_parameters, lhs);
	}
}

void
Whole_program::analyse_method_info (Method_info* method_info,
												Basic_block* caller,
												MIR::Actual_parameter_list* actuals,
												MIR::VARIABLE_NAME* lhs)
{
	if (method_info->has_implementation ())
	{
		User_method_info* info = dyc<User_method_info> (method_info);
		if (info->cfg == NULL)
			info->cfg = new CFG (info->method);

		analyse_function (info, caller, actuals, lhs);
	}
	else
	{
		// Get as precise information as is possible with pre-baked summary
		// information.
		analyse_summary (dyc<Summary_method_info> (method_info), caller, actuals, lhs);
	}
}

void
Whole_program::analyse_summary (Summary_method_info* info, Basic_block* caller, Actual_parameter_list* actuals, VARIABLE_NAME* lhs)
{
	CFG* cfg = info->get_cfg ();
	Basic_block* fake = info->get_fake_bb ();


	// Start the analysis
	forward_bind (info, caller, cfg->get_entry_bb(), actuals);

	// Create OUT sets for the entry node
	foreach_wpa (this)
		wpa->aggregate_results (cfg->get_entry_bb ());



	/*
	 * "Perform" the function
	 */

	pull_results (fake);

	// TODO: its difficult to know exactly what this representation should
	// look like when we haven't tried modelling that many functions. Instead,
	// we'll write 'baked-functions', which model it by directly calling
	// Whole_program methods. When we've done a few of these, it should be a
	// lot clearer what we want to model here (also, this allows us model hard
	// functions which might not be modelled with a data approach).
	apply_modelled_function (info, fake);

	// Create OUT sets from the results 
	foreach_wpa (this)
		wpa->aggregate_results (fake);



	/*
	 * Backward bind
	 */

	pull_results (cfg->get_exit_bb ());

	backward_bind (info, caller, cfg->get_exit_bb (), lhs);
}

// BB is the block representing the whole method
void
Whole_program::apply_modelled_function (Method_info* info, Basic_block* bb)
{
	// TODO:
	//	If we know all the values for all the parameters, and the function has
	//	no side-effects, call the function on its parameters.
	//
	//	TODO: stop only modelling types.

	Path* ret_name = P (ST(bb), new VARIABLE_NAME (RETNAME));
	if (*info->name == "strlen")
	{
		assign_typed (bb, ret_name, Types ("int"));
	}
	else if (*info->name == "dechex")
	{
		assign_typed (bb, ret_name, Types ("string"));
	}
	else if (*info->name == "print")
	{
		assign_scalar (bb, ret_name, new INT (1));
	}
	else if (*info->name == "is_array")
	{
		assign_typed (bb, ret_name, Types ("bool"));
	}
	else if (*info->name == "is_object")
	{
		assign_typed (bb, ret_name, Types ("bool"));
	}
	else if (*info->name == "trigger_error")
	{
		assign_typed (bb, ret_name, Types ("bool"));
	}
	else
		phc_TODO ();
}

void
Whole_program::apply_results (User_method_info* info)
{
	// Since we use information from lots of sources, and we need this
	// information for tons of differernt optimizations, its best to have a
	// single transformer applying the results.
	foreach (Basic_block* bb, *info->cfg->get_all_bbs ())
	{
		// TODO: I should probably use CCP results here to optimize branches.
		if (Statement_block* sb = dynamic_cast<Statement_block*> (bb))
		{
			Statement* old = sb->statement->clone ();

			transformer->visit_block (bb);

			if (sb->statement->equals (old))
				DEBUG ("No changes in BB: " << bb->ID);
			else
				DEBUG ("BB " << bb->ID << " changed");
		}

	}
	if (debugging_enabled)
		info->cfg->dump_graphviz (s("Apply results"));
}

void
Whole_program::annotate_results (User_method_info* info)
{
	// Since we use information from lots of sources, and we need this
	// information for tons of different annotations, its best to have a
	// single annotator applying the results.
	foreach (Basic_block* bb, *info->cfg->get_all_bbs ())
		annotator->visit_block (bb);
}

void
Whole_program::perform_local_optimizations (User_method_info* info)
{
	pm->run_local_optimization_passes (this, info->cfg);

	pm->maybe_enable_debug (s("wpa"));
}

void
Whole_program::perform_interprocedural_optimizations (User_method_info* info)
{
	pm->run_ipa_passes (this, info->cfg);

	pm->maybe_enable_debug (s("wpa"));
}

void
Whole_program::strip (MIR::PHP_script* in)
{
	in->transform_children (new Method_pruner ());
}

void
Whole_program::generate_summary (User_method_info* info)
{
	// Simplest possible inlining info - the function does nothing.
	if (info->cfg->get_all_bbs ()->size() == 2)
		info->side_effecting = true;
}

void
Whole_program::merge_contexts (User_method_info* info)
{
	// TODO: once we have a function that's called from multiple different
	// places.
}

bool
Whole_program::analyse_block (Basic_block* bb)
{
	DEBUG ("Analysing BB: " << bb->ID);

	// Merge results from predecessors
	pull_results (bb);


	// Perform analyses
	visit_block (bb);


	// Create OUT sets from the results 
	foreach_wpa (this)
		wpa->aggregate_results (bb);

	dump (bb, "After analysis");

	// Calculate fix-point
	bool changed = false;
	foreach_wpa (this)
		changed |= wpa->solution_changed (bb);

	return changed;
}

void
Whole_program::pull_results (Basic_block* bb)
{
	foreach_wpa (this)
	{
		wpa->pull_init (bb);

		bool first = true;
		foreach (Edge* pred, *bb->get_predecessor_edges ())
		{
			// Only merge from executable edges
			if (!pred->is_executable)
				continue;

			if (first)
			{
				wpa->pull_first_pred (bb, pred->get_source ());
				first = false;
			}
			else
			{
				wpa->pull_pred (bb, pred->get_source ());
			}
		}

		wpa->pull_finish (bb);
	}
}

void
Whole_program::dump (Basic_block* bb, string comment)
{
	CHECK_DEBUG ();
	foreach_wpa (this)
	{
		// This isnt the greatest means of debugging.
		pm->maybe_enable_debug (s(wpa->name));

		if (!debugging_enabled)
			continue;

		DEBUG (bb->ID << " (" << comment << "): Dumping " << wpa->name);
		wpa->dump (bb, comment);
		cdebug << endl;
	}
	pm->maybe_enable_debug (s("wpa"));
}


/*
 * Analysis from here on in
 */

void
Whole_program::init_superglobals (Entry_block* entry)
{
	// TODO: Strictly speaking, functions other than __MAIN__ should have their
	// globals set up before the parameters are copied. However, we'll ignore
	// this minor bug since its broken elsewhere in the compiler.

	// TODO: add HTTP_*
	
	// TODO: we incorrectly mark _SERVER as being an array of strings. However,
	// it actually has "argc", "argv" and "REQUEST_TIME" set, which are not strings.
	

	// Start with globals, since it needs needs to point to MSN
	assign_empty_array (entry, P (MSN, new VARIABLE_NAME ("GLOBALS")), MSN);

	// Do the other superglobals
	foreach (VARIABLE_NAME* sg, *PHP::get_superglobals ())
	{
		if (*sg->value == "GLOBALS")
			continue;

		// TODO: we mark them as arrays of strings, but in reality we only know
		// this about some of them.

		// Create an empty array
		string array_name = *sg->value;
		assign_empty_array (entry, P (MSN, array_name), array_name);

		// We dont know the contents of these arrays.
		// TODO: move all of these into calls to Whole_program
		assign_typed (entry, P (array_name, UNKNOWN), Types("string"));
	}

	// We actually have no idea whats in _SESSION
	assign_unknown (entry, P ("_SESSION", UNKNOWN));

	// argc
	assign_typed (entry, P (MSN, "argc"), Types ("int"));

	// argv
	assign_empty_array (entry, P (MSN, "argv"), "argv");
	assign_typed (entry, P ("argv", UNKNOWN), Types ("string"));
	assign_typed (entry,  P("argv", "0"), Types("string"));

	dump (entry, "After superglobals");
}

void
Whole_program::forward_bind (Method_info* info, Basic_block* caller, Entry_block* entry, MIR::Actual_parameter_list* actuals)
{
	// Each caller should expect that context can be NULL for __MAIN__.
	foreach_wpa (this)
		wpa->forward_bind (caller, entry);

	// Special case for __MAIN__. We do it here so that the other analyses
	// have initialized.
	if (caller == NULL)
	{
		init_superglobals (entry);
	}


	int i = 0;
	foreach (Actual_parameter* ap, *actuals)
	{
		if (ap->is_ref || info->param_by_ref (i))
		{
			// $ap =& $fp;
			assign_by_ref (entry,
					P (ST (entry), info->param_name (i)),
					P (ST (caller), dyc<VARIABLE_NAME> (ap->rvalue)));
		}
		else
		{
			// $ap = $fp;
			if (isa<VARIABLE_NAME> (ap->rvalue))
			{
				assign_by_copy (entry,
						P (ST (entry), info->param_name (i)),
						P (ST (caller), dyc<VARIABLE_NAME> (ap->rvalue)));
			}
			else
			{
				assign_scalar (entry,
						P (ST (entry), info->param_name (i)),
						dyc<Literal> (ap->rvalue));
			}
		}

		i++;
	}

	// Default values
	while (true)
	{
		if (info->default_param (i))
			phc_TODO ();
//	if (fp->var->default_value)
		else
			break;
	}

	foreach_wpa (this)
		wpa->aggregate_results (entry);

	dump (entry, "After forward_bind");
}


void
Whole_program::backward_bind (Method_info* info, Basic_block* caller, Exit_block* exit, MIR::VARIABLE_NAME* lhs)
{
	// Do assignment back to LHS
	//
	// If we do the assignment in the caller, then it will use the result from
	// the IN of the caller, which wont tell us anything. It should use the out
	// of the callee. However, using the callee means we need to ensure the
	// results have propagated. So the callee has 3 BBs: entry, exit and the one
	// where the work is done.
	//
	// The assignment to LHS is done in the context of the callee, and then the
	// results are backwards_bound. This has the added advantage that we can
	// strip the callees results from the solution without worrying. There is a
	// danger that it might make an analysis think that return value somehow
	// escapes. I'm not sure if anything needs to be done about that.

	if (lhs)
	{
		if (info->return_by_ref ())
		{
			// $lhs =& $retval;
			assign_by_ref (exit,
					P (ST (caller), dyc<VARIABLE_NAME> (lhs)),
					P (ST (exit), new VARIABLE_NAME (RETNAME)));
		}
		else
		{
			// $lhs = $retval;
			assign_by_copy (exit,
					P (ST (caller), dyc<VARIABLE_NAME> (lhs)),
					P (ST (exit), new VARIABLE_NAME (RETNAME)));
		}
	}

	// Context can be NULL for __MAIN__
	foreach_wpa (this)
		wpa->backward_bind (caller, exit);

	if (caller)
		dump (caller, "After backward bind");
}


/*
 * Use whatever information is available to determine the assignments which
 * occur here.
 */

bool
is_must (Index_node_list* indices)
{
	// If the edge between it and its storage node is POSSIBLE, this function
	// is still correct. All that matters is whether we can refer to one index
	// node, or multiple.
	assert (!indices->empty ());
	return (indices->size () == 1);
}

// Returns the certainty with which assignments can be made to it.
certainty
Whole_program::kill_value (Basic_block* bb, Path* plhs)
{
	Index_node_list* lhss = get_named_indices (bb, plhs);

	// TODO: dont kill fields of abstract storage nodes

	// Don't kill if this refers to more than 1 index_node, which means we
	// don't know what variable to kill.
	if (!is_must (lhss))
		return POSSIBLE;

	// Fetch each reference of LHS, and kill them.
	Index_node* lhs = lhss->front ();
	
	// Don't kill may-refs
	foreach (Index_node* ref, *aliasing->get_references (bb, lhs, DEFINITE))
		foreach_wpa (this)
			wpa->kill_value (bb, ref->name ());

	// Handle LHS itself
	foreach_wpa (this)
		wpa->kill_value (bb, lhs->name ());

	return DEFINITE;
}

void
Whole_program::assign_by_ref (Basic_block* bb, Path* plhs, Path* prhs)
{
	// Should we separate the assignment by value and the assignment by ref.
	phc_TODO ();
	Index_node_list* lhss = get_named_indices (bb, plhs);
	Index_node_list* rhss = get_named_indices (bb, prhs, true);

	bool killable = is_must (lhss);

	// Send the results to the analyses for all variables which could be
	// overwritten.
	foreach (Index_node* lhs, *lhss)
	{
		if (killable) // only 1 result
		{
			foreach_wpa (this)
				wpa->kill_reference (bb, lhs->name ());
		}

		// We don't need to worry about propagating values to LHSS' aliases, as
		// the aliasing relations are killed above.
	
		foreach (Index_node* rhs, *rhss)
		{
			foreach_wpa (this)
			{
				wpa->create_reference (bb,
					lhs->name (),
					rhs->name (),
					(killable && is_must (rhss)) ? DEFINITE : POSSIBLE);

				phc_TODO ();
//				wpa->assign_value (bb,
//					lhs->name (),
//					rhs->name (),
//					(killable && is_must (rhss)) ? DEFINITE : POSSIBLE);
			}
		}
	}
}


void
Whole_program::assign_scalar (Basic_block* bb, Path* plhs, Literal* lit)
{
	certainty cert = kill_value (bb, plhs);
	foreach (Alias_name name, *get_all_referenced_names (bb, plhs, cert))
	{
		foreach_wpa (this)
		{
			wpa->assign_scalar (bb, name, ABSVAL (name)->name(),
					Abstract_value::from_literal (lit), POSSIBLE);
		}
	}
}

void
Whole_program::assign_typed (Basic_block* bb, Path* plhs, Types types)
{
	// Split scalars, objects and arrays here.
	Types scalars = Type_inference::get_scalar_types (types);
	Types array = Type_inference::get_array_types (types);
	Types objects = Type_inference::get_object_types (types);

	certainty cert = kill_value (bb, plhs);
	foreach (Alias_name name, *get_all_referenced_names (bb, plhs, cert))
	{
		foreach_wpa (this)
		{
			if (scalars.size ())
				wpa->assign_scalar (bb, name, ABSVAL (name)->name(),
						Abstract_value::from_types (scalars), POSSIBLE);

			if (array.size ())
				phc_TODO ();

			if (objects.size ())
				phc_TODO ();

			//	wpa->assign_storage (bb, ref->name(),
			//								BB_array_name (bb)->name(), POSSIBLE);

			//	wpa->assign_storage (bb, ref->name(),
			//								BB_object_name (bb)->name(), POSSIBLE);
		}
	}
}

void
Whole_program::assign_empty_array (Basic_block* bb, Path* plhs, string unique_name)
{
	certainty cert = kill_value (bb, plhs);
	foreach (Alias_name name, *get_all_referenced_names (bb, plhs, cert))
	{
		foreach_wpa (this)
		{
			wpa->assign_empty_array (bb, name, unique_name, cert);
		}
	}
}

void
Whole_program::assign_unknown (Basic_block* bb, Path* plhs)
{
	// This assigns a value which is unknown, but is not as bad as
	// ruin_everything (ie, it doesnt link to all the other objects, arrays,
	// etc. Is this being used right?

	certainty cert = kill_value (bb, plhs);

	// Unknown may be an array, a scalar or an object, all of which have
	// different properties. We must be careful to separate these.
	foreach (Alias_name name, *get_all_referenced_names (bb, plhs, cert))
	{
		// When assigning to different references:
		//		- scalar values are copied (though they are conceptually shared,
		//		we deal with that through functions like this).
		//		- the array is shared, not copied. It will have a unique name.
		//		- the object is shared, and will have a unique name.
		foreach_wpa (this)
		{
			// TODO should these be empty?
			wpa->assign_scalar (bb, name, ABSVAL (name)->name(),
									  Abstract_value::unknown (), POSSIBLE);
			wpa->assign_storage (bb, name, BB_array_name (bb)->name(), POSSIBLE);
			wpa->assign_storage (bb, name, BB_object_name (bb)->name(), POSSIBLE);
		}
	}
}



void
Whole_program::assign_by_copy (Basic_block* bb, Path* plhs, Path* prhs)
{
	// foreach values V pointed to by PRHS:
	//	switch V.type:
	//		Scalar:
	//			- foreach alias A of PLHS, set the value of A::ABSVAL using V.
	//		Array:
	//			- foreach alias A of PLHS, create a copy of V, with a new name.
	//		Objects:
	//			- foreach alias A of PLHS, point from A to V.

	certainty cert = kill_value (bb, plhs);

	// For objects, copy the edge. For arrays, copy the whole thing. For
	// scalars, copy the scalar (if unknown). It seems clear that we need an
	// unknown object here, if the type is not known to not be an object.
	Index_node_list* rhss = get_named_indices (bb, prhs, true);

	foreach (Alias_name name, *get_all_referenced_names (bb, plhs, cert))
	{
		foreach_wpa (this)
		{
			foreach (Index_node* rhs, *rhss)
			{
				// Get the value for each RHS. Copy it using the correct semantics.

				// TODO: I think we're losing some CERT information
				Storage_node_list* values = aliasing->get_values (bb, rhs, PTG_ALL);
				foreach (Storage_node* st, *values)
				{
					// Get the type of the value
					Types types = type_inf->get_types (bb, st->name());
					// TODO: handle bottom

					// It must be either all scalars, array, list of classes, or bottom.
					Types scalars = Type_inference::get_scalar_types (types);
					Types array = Type_inference::get_array_types (types);
					Types objects = Type_inference::get_object_types (types);

					assert (!scalars.empty() ^ !array.empty() ^ !objects.empty());

					if (scalars.size())
					{
						wpa->assign_scalar (bb, name, ABSVAL (name)->name(),
								get_abstract_value (bb, st->name()), POSSIBLE);
					}

					if (array.size())
					{
						phc_TODO ();
					}

					if (objects.size ())
					{
						phc_TODO ();
					}
				}
			}
		}
	}
}

void
Whole_program::record_use (Basic_block* bb, Index_node* index_node)
{
	// TODO: this marks it as a use, not a must use. Is there any difference
	// as far as analyses are concerned? If so, fix this. If not, remove the
	// may-uses.

	// TODO: once type-inferences is built, here would be a good place to
	// call/check for the handlers.
	
	foreach_wpa (this)
		wpa->record_use (bb, index_node->name(), POSSIBLE);
}



void
Whole_program::ruin_everything (Basic_block* bb, Path* plhs)
{
	// For every storage node we can reach, mark its "*" index as completely
	// unknown.
	phc_TODO ();
}




/*
 * Return the range of possible values for INDEX. This is used to
 * disambiguate for indexing other nodes. It returns a set of strings. If
 * only 1 string is returned, it must be that value. If more than one strings
 * are returned, it may be any of them. NULL may be returned, indicating that it may be all possible values.
 */
String_list*
Whole_program::get_string_values (Basic_block* bb, Index_node* index)
{
	Lattice_cell* result = ccp->get_value (bb, index->name ());

	if (result == TOP)
		return new String_list (s(""));

	if (result == BOTTOM)
		return new String_list (s(UNKNOWN));

	// TODO: this isnt quite right, we need to cast to a string.
	return new String_list (
		dyc<Literal_cell> (result)->value->get_value_as_string ());
}


Abstract_value*
Whole_program::get_abstract_value (Basic_block* bb, Alias_name name)
{
	return new Abstract_value (
							ccp->get_value (bb, name), 
							type_inf->get_value (bb, name));
}

Abstract_value*
Whole_program::get_bb_out_abstract_value (Basic_block* bb, Alias_name name)
{
	return new Abstract_value (
							ccp->outs[bb->ID][name.str()],
							type_inf->outs[bb->ID][name.str()]);
}



/*
 * Return the set of names which PATH might lead to.
 *
 * Its also a little bit of a catch-all function. Since it processes uses of
 * index_nodes, it must mark them as used, and check types to see if there
 * are any handlers that need to be called. It checks CCP to see the range of
 * variables that might be looked up, and any other analysis which can reduce
 * the range of the results.
 *
 * Suppose we get a single result, x. Can we say that a def to this must-def x?
 *		- I believe that scalars cant affect this
 *		- I think we can say that.
 *
 *
 *	TODO: there is a bit of a problem here with implicit creation of values. If
 *	we're looking to the the assignment $x[$i] = 5, we need to create $x.
 *	Likewise for $y =& $x[$i] or anything in the form $y =& $x->$f.
 */
Index_node_list*
Whole_program::get_named_indices (Basic_block* bb, Path* path, bool record_uses)
{
	Indexing* p = dyc<Indexing> (path);


	// Get the set of storage nodes representing the LHS.
	Set<string> lhss;

	if (ST_path* st = dynamic_cast <ST_path*> (p->lhs))
	{
		// 1 named storage node
		lhss.insert (st->name);
	}
	else
	{
		// TODO: propagate record_uses?
		// Lookup the storage nodes indexed by LHS
		foreach (Index_node* st_index, *get_named_indices (bb, p->lhs, record_uses))
		{
			foreach (Storage_node* pointed_to,
						*aliasing->get_values (bb, st_index, PTG_ALL))
			{
				string name = pointed_to->storage;

				// If this is a scalar, we have to deal with implicit creation.
				if (pointed_to->storage == "SCALAR")
				{
					name = lexical_cast<string> (bb->ID);
					assign_empty_array (bb, p->lhs, name);
					// TODO: what if the array being implicitly created is a
					// scalar? in that case the conversion wont happen.
					// TODO: What if its a string? This wont create an array in that
					// case.
					// TODO: we cant tell for sure that this implicit conversion
					// will work. What if the scalar is 5?
				}

				lhss.insert (name);
			}
		}
	}


	// Get the names of the fields of the storage nodes.
	Set<string> rhss;

	if (Index_path* st = dynamic_cast <Index_path*> (p->rhs))
	{
		// 1 named field of the storage nodes
		rhss.insert (st->name);
	}
	else
	{
		// The name of the field must be looked up
		foreach (Index_node* field_index, *get_named_indices (bb, p->rhs, record_uses))
		{
			// Record this use regardless of RECORD_USES
			record_use (bb, field_index);

			// This should return a set of possible names, 1 known name (including
			// "*" indicating it could be anything).
			foreach (String* value, *get_string_values (bb, field_index))
				rhss.insert (*value);
		}
	}

	assert (rhss.size ());


	// Combine the results
	Index_node_list* result = new Index_node_list;

	foreach (string lhs, lhss)
		foreach (string rhs, rhss)
		{
			if (record_uses)
				record_use (bb, new Index_node (lhs, rhs));

			result->push_back (new Index_node (lhs, rhs));
		}

	return result;
}

Index_node*
Whole_program::get_named_index (Basic_block* bb, Path* name, bool record_uses)
{
	Index_node_list* all = get_named_indices (bb, name, record_uses);

	// TODO: can this happen
	assert (all->size());

	if (all->size () > 1)
		return NULL;

	return all->front();
}



List<Alias_name>*
Whole_program::get_all_referenced_names (Basic_block* bb, Path* path, certainty cert, bool record_uses)
{
	Set<Alias_name> names;

	Index_node_list* lhss = get_named_indices (bb, path, record_uses);

	foreach (Index_node* lhs, *lhss)
	{
		// Handle all the aliases/indirect assignments.
		Index_node_list* refs = aliasing->get_references (bb, lhs, cert);
		refs->push_back (lhs);

		foreach (Index_node* ref, *refs)
		{
			names.insert (ref->name());
		}
	}

	return names.to_list();
}

/*
 * Analysis
 */

void
Whole_program::visit_global (Statement_block* bb, MIR::Global* in)
{
	assign_by_ref (bb,
			P (ST (bb), in->variable_name),
			P ("__MAIN__", in->variable_name));
}


void
Whole_program::visit_assign_array (Statement_block* bb, MIR::Assign_array* in)
{
	string ns = ST (bb);
	Path* lhs = P (ns, in);
	Path* rhs = P (ns, in->rhs);

	if (in->is_ref)
		assign_by_ref (bb, lhs, rhs);
	else
		assign_by_copy (bb, lhs, rhs);
}


void
Whole_program::visit_foreach_reset (Statement_block* bb, MIR::Foreach_reset* in)
{
	// mark the array as used
	record_use (bb, VN (ST(bb), in->array));

	// Mark iterator as defined. The iterator does nothing for us otherwise.
	Alias_name iter (ST(bb), *in->iter->value);
	
	// We dont use Whole_programm::assign_unknown because we havent got a Path
	// for an iterator. We also don't need to worry about kills and such. Note
	// that we dont want a path, as that would create an index into the
	// array's storage node, which isnt what we want to model.
	phc_TODO ();
/*	foreach_wpa (this)
		wpa->assign_unknown (bb, iter, DEFINITE);*/
}

void
Whole_program::visit_foreach_end (Statement_block* bb, MIR::Foreach_end* in)
{
	// Mark the array as used
	record_use (bb, VN (ST(bb), in->array));

	// Mark both a use and a def on the iterator
	Alias_name iter (ST(bb), *in->iter->value);
	record_use (bb, iter.ind());
	phc_TODO ();

/*	foreach_wpa (this)
		wpa->assign_unknown (bb, iter, DEFINITE);*/
}


void
Whole_program::visit_assign_var (Statement_block* bb, MIR::Assign_var* in)
{
	saved_plhs = P (ST(bb), in->lhs);
	saved_lhs = in->lhs;

	switch (in->rhs->classid())
	{
		case Array_access::ID:
		case Bin_op::ID:
		case Cast::ID:
		case Constant::ID:
		case Field_access::ID:
		case Foreach_get_key::ID:
		case Foreach_get_val::ID:
		case Foreach_has_key::ID:
		case Instanceof::ID:
		case Isset::ID:
		case Method_invocation::ID:
		case New::ID:
		case Param_is_ref::ID:
		case Unary_op::ID:
		case VARIABLE_NAME::ID:
		case Variable_variable::ID:
			visit_expr (bb, in->rhs);
			break;

		// Values
		case BOOL::ID:
		case INT::ID:
		case NIL::ID:
		case REAL::ID:
		case STRING::ID:
			assign_scalar (bb, saved_plhs, dyc<Literal> (in->rhs));
			break;

		default:
			phc_unreachable ();
			break;
	}

	saved_lhs = NULL;
	saved_plhs = NULL;
}

void
Whole_program::visit_eval_expr (Statement_block* bb, MIR::Eval_expr* in)
{
	saved_plhs = NULL;
	saved_lhs = NULL;
	visit_expr (bb, in->expr);
}

void
Whole_program::visit_unset (Statement_block* bb, MIR::Unset* in)
{
	// TODO: remove references here, not just values.

	// Get the index nodes. Remove them.
	Path* path = P (ST(bb), in);

	// This isnt quite right - there are references to take care of
	assign_scalar (bb, path, new NIL);
}

void
Whole_program::visit_branch_block (Branch_block* bb)
{
	record_use (bb, VN (ST(bb), bb->branch->variable_name));
}


void
Whole_program::visit_pre_op (Statement_block* bb, Pre_op* in)
{
	// ++ and -- won't affect objects.
	Path* path = P (ST(bb), in->variable_name);

	// I'm not really sure how to get a good interface on all this.
	Index_node* n = VN (ST(bb), in->variable_name);

	// Case where we know the value
	MIR::Literal* value = ccp->get_lit (bb, n->name());
	if (value)
	{
		Literal* result = PHP::fold_pre_op (value, in->op);
		assign_scalar (bb, path, result);
		return;
	}

	// Maybe we know the type?
	Type_cell* tc = dyc<Type_cell> (type_inf->get_value (bb, n->name()));
	assert (tc != TOP); // would be NULL in CCP
	if (tc == BOTTOM)
	{
		assign_unknown (bb, path);
		return;
	}

	assign_typed (bb, path, tc->types);
}

void
Whole_program::visit_assign_field (Statement_block*, MIR::Assign_field*)
{
	phc_TODO ();
}

void
Whole_program::visit_assign_var_var (Statement_block*, MIR::Assign_var_var*)
{
	phc_TODO ();
}

void
Whole_program::visit_foreach_next (Statement_block*, MIR::Foreach_next*)
{
	phc_TODO ();
}

void
Whole_program::visit_assign_next (Statement_block*, MIR::Assign_next*)
{
	phc_TODO ();
}

void
Whole_program::visit_return (Statement_block*, MIR::Return*)
{
	phc_TODO ();
}

void
Whole_program::visit_static_declaration (Statement_block*, MIR::Static_declaration*)
{
	phc_TODO ();
}

void
Whole_program::visit_throw (Statement_block*, MIR::Throw*)
{
	phc_TODO ();
}

void
Whole_program::visit_try (Statement_block*, MIR::Try*)
{
	phc_TODO ();
}


void
Whole_program::visit_array_access (Statement_block* bb, MIR::Array_access* in)
{
	phc_TODO ();
}

void
Whole_program::visit_bin_op (Statement_block* bb, MIR::Bin_op* in)
{
	Abstract_value* left = get_abstract_value (bb, in->left);
	Abstract_value* right = get_abstract_value (bb, in->right);

	if (isa<Literal_cell> (left) && isa<Literal_cell> (right))
	{
		Literal* result = PHP::fold_bin_op (dyc<Literal_cell> (left)->value,
														in->op,
														dyc<Literal_cell> (right)->value);
		assign_scalar (bb, saved_plhs, result);
		return;
	}

	// TODO: record uses

	Types types = type_inf->get_bin_op_types (bb, left, right, *in->op->value);

	assign_typed (bb, saved_plhs, types);
}

Abstract_value*
Whole_program::get_abstract_value (Basic_block* bb, MIR::Rvalue* rval)
{
	if (isa<Literal> (rval))
		return Abstract_value::from_literal (dyc<Literal> (rval));

	// The variables are not expected to already have the same value. Perhaps
	// there was an assignment to $x[0], and we are accessing $x[$i].
	Index_node_list* indices = get_named_indices (bb, P (ST(bb), dyc<VARIABLE_NAME> (rval)));

	if (indices->size () > 1)
		phc_TODO ();
	
	return get_abstract_value (bb, indices->front()->name());
}

void
Whole_program::visit_cast (Statement_block* bb, MIR::Cast* in)
{
	Alias_name operand = VN (ST(bb), in->variable_name)->name();

	MIR::Literal* lit = ccp->get_lit (bb, operand);
	if (lit)
	{
		Literal* result = PHP::cast_to (in->cast, lit);
		if (result)
		{
			assign_scalar (bb, saved_plhs, result);
			return;
		}
	}

	// We've handled casts for known scalars to scalars. We still must handle
	// casts to objects, casts to arrays, and casts from unknown values to
	// other scalar types.
	assign_typed (bb, saved_plhs, Types ("array"));

/*	foreach (Storage_node* pointed_to,
					*aliasing->get_values (bb, operand.ind(), PTG_ALL))
	{
		cdebug << pointed_to->name().str();
	}
	phc_TODO ();*/

	phc_TODO ();
}

void
Whole_program::visit_constant (Statement_block* bb, MIR::Constant* in)
{
	phc_TODO ();

	Literal* lit = PHP::fold_constant (in);
	if (lit)
	{
		assign_scalar (bb, saved_plhs, lit);
		return;
	}

	// Assign_unknown_typed (Types (sitrng, bool, null, etc
	phc_TODO ();
}

void
Whole_program::visit_field_access (Statement_block* bb, MIR::Field_access* in)
{
	phc_TODO ();
}

void
Whole_program::visit_foreach_get_key (Statement_block* bb, MIR::Foreach_get_key* in)
{
	phc_TODO ();
}

void
Whole_program::visit_foreach_get_val (Statement_block* bb, MIR::Foreach_get_val* in)
{
	phc_TODO ();
}

void
Whole_program::visit_foreach_has_key (Statement_block* bb, MIR::Foreach_has_key* in)
{
	phc_TODO ();
}

void
Whole_program::visit_instanceof (Statement_block* bb, MIR::Instanceof* in)
{
	phc_TODO ();
}

void
Whole_program::visit_isset (Statement_block* bb, MIR::Isset* in)
{
	phc_TODO ();
}

void
Whole_program::visit_method_invocation (Statement_block* bb, MIR::Method_invocation* in)
{
	invoke_method (in, bb, saved_lhs);
}

void
Whole_program::visit_new (Statement_block* bb, MIR::New* in)
{
	phc_TODO ();
}

void
Whole_program::visit_param_is_ref (Statement_block* bb, MIR::Param_is_ref* in)
{
	phc_TODO ();
}

void
Whole_program::visit_unary_op (Statement_block* bb, MIR::Unary_op* in)
{
	phc_TODO ();
}

void
Whole_program::visit_variable_name (Statement_block* bb, MIR::VARIABLE_NAME* in)
{
	assign_by_copy (bb, saved_plhs, P (ST(bb), in));
}

void
Whole_program::visit_variable_variable (Statement_block* bb, MIR::Variable_variable* in)
{
	phc_TODO ();
}
