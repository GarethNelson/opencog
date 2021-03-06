/*
 * @file opencog/embodiment/Control/OperationalAvatarController/PsiActionSelectionAgent.cc
 *
 * @author Jinhua Chua <JinhuaChua@gmail.com>
 * @date 2011-12-28
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <boost/tokenizer.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>

#include <opencog/embodiment/AtomSpaceExtensions/atom_types.h>
#include <opencog/nlp/types/atom_types.h>
#include <opencog/spacetime/atom_types.h>
#include <opencog/spacetime/SpaceServer.h>
#include <opencog/util/platform.h>
#include <opencog/util/random.h>

#include "OAC.h"
#include "PsiActionSelectionAgent.h"
#include "PsiRuleUtil.h"

using namespace opencog::oac;

extern int currentDebugLevel;

PsiActionSelectionAgent::~PsiActionSelectionAgent()
{

}

PsiActionSelectionAgent::PsiActionSelectionAgent(CogServer& cs)
	: Agent(cs), oac(dynamic_cast<OAC&>(cs)),
	  atomspace(oac.getAtomSpace()), cycleCount(0)
{
    // Force the Agent initialize itself during its first cycle.
    this->forceInitNextCycle();
}

void PsiActionSelectionAgent::init()
{
    logger().debug("PsiActionSelectionAgent::%s - "
                   "Initializing the Agent [cycle = %d]",
                   __FUNCTION__, this->cycleCount);

    // Initialize the list of Demand Goals
    this->initDemandGoalList();

    // Initialize other members
    this->currentSchemaId = 0;
    this->procedureExecutionTimeout =
	    config().get_long("PROCEDURE_EXECUTION_TIMEOUT");

    // Avoid initialize during next cycle
    this->bInitialized = true;
}

void PsiActionSelectionAgent::initDemandGoalList()
{
    logger().debug("PsiActionSelectionAgent::%s - "
                   "Initializing the list of Demand Goals (Final Goals) "
                   "[ cycle =%d ]",
                    __FUNCTION__, this->cycleCount);

    // Clear the old demand goal list
    this->psi_demand_goal_list.clear();

    // Get demand names from the configuration file
    std::string demandNames = config()["PSI_DEMANDS"];

    // Process Demands one by one
    boost::tokenizer<> demandNamesTok (demandNames);

    std::string demandPredicateName;
    HandleSeq outgoings;

    for ( boost::tokenizer<>::iterator iDemandName = demandNamesTok.begin();
          iDemandName != demandNamesTok.end();
          ++ iDemandName ) {

        demandPredicateName = (*iDemandName) + "DemandGoal";

        // Get EvaluationLink to the demand goal
        outgoings = { atomspace.addNode(PREDICATE_NODE, demandPredicateName) };

        this->psi_demand_goal_list.push_back(atomspace.addLink(EVALUATION_LINK,
                                                               outgoings));

    }// for

    // Create an ReferenceLink holding all the demand goals (EvaluationLink)
    outgoings = { atomspace.addNode(CONCEPT_NODE, "psi_demand_goal_list"),
                  atomspace.addLink(LIST_LINK, this->psi_demand_goal_list) };
    Handle referenceLink = AtomSpaceUtil::addLink(atomspace, REFERENCE_LINK,
                                                  outgoings, true);

    logger().debug("PsiActionSelectionAgent::%s - "
                   "Add the list of demand goals to AtomSpace: %s [cycle = %d]",
                   __FUNCTION__, atomspace.atomAsString(referenceLink).c_str(),
                   this->cycleCount);
}

void PsiActionSelectionAgent::getActions(Handle hStep, HandleSeq & actions)
{
    opencog::Type atomType = atomspace.getType(hStep);

    if (atomType == EXECUTION_LINK) {
        actions.insert(actions.begin(), hStep);
    }
    else if (atomType == AND_LINK ||
            atomType == SEQUENTIAL_AND_LINK) {
        for ( Handle hOutgoing : atomspace.getOutgoing(hStep) ) {
            this->getActions(hOutgoing, actions);
        }
    }
    else if (atomType == OR_LINK ) {
        const HandleSeq & outgoings = atomspace.getOutgoing(hStep);
        Handle hRandomSelected = rand_element(outgoings);
        this->getActions(hRandomSelected, actions);
    }
}

bool PsiActionSelectionAgent::getPlan()
{
    // Check the state of latest planning
    HandleSeq tempOutgoingSet =
        { atomspace.addNode(PREDICATE_NODE, "plan_success"),
          atomspace.addLink(LIST_LINK, HandleSeq()) };

    Handle hPlanSuccessEvaluationLink = atomspace.addLink(EVALUATION_LINK,
                                                          tempOutgoingSet);
    if ( atomspace.getTV(hPlanSuccessEvaluationLink)->getMean() < 0.9 )
        return false;

    // Get the planning result
    Handle hSelectedDemandGoal =
        AtomSpaceUtil::getReference(atomspace,
                                    atomspace.getHandle(CONCEPT_NODE,
                                                        "plan_selected_demand_goal"
                                                       )
                                   );

    this->plan_selected_demand_goal = hSelectedDemandGoal;

    Handle hRuleList =
        AtomSpaceUtil::getReference(atomspace,
                                    atomspace.getHandle(CONCEPT_NODE,
                                                        "plan_rule_list"
                                                       )
                                   );

    this->plan_rule_list = atomspace.getOutgoing(hRuleList);

    Handle hContextList =
        AtomSpaceUtil::getReference(atomspace,
                                    atomspace.getHandle(CONCEPT_NODE,
                                                        "plan_context_list"
                                                       )
                                   );

    this->plan_context_list = atomspace.getOutgoing(hContextList);

    Handle hActionList =
        AtomSpaceUtil::getReference(atomspace,
                                    atomspace.getHandle(CONCEPT_NODE,
                                                        "plan_action_list"
                                                       )
                                   );

    this->plan_action_list = atomspace.getOutgoing(hActionList);
    this->temp_action_list = this->plan_action_list;

    return true;
}

void PsiActionSelectionAgent::printPlan()
{
    std::cout << std::endl << "Selected Demand Goal [cycle = "
              << this->cycleCount << "]:" << std::endl
              << atomspace.atomAsString(this->plan_selected_demand_goal)
              << std::endl;

    int i = 1;

    for ( const Handle hAction : this->plan_action_list ) {
        std::cout << std::endl << "Step No."<< i
                  << std::endl << atomspace.atomAsString(hAction);

        ++i;
    }

    std::cout << std::endl << std::endl;
}

void PsiActionSelectionAgent::stimulateAtoms()
{
    for (Handle h : this->plan_rule_list)
        this->stimulateAtom(h, 10); // 10 is the same as noiseUnit in ImportanceUpdatingAgent

    for (Handle h : this->plan_context_list)
        this->stimulateAtom(h, 10);

    for (Handle h : this->plan_action_list)
        this->stimulateAtom(h, 10);

    logger().warn("PsiActionSelectionAgent::%s - Stimulate plan related atoms [cycle = %d]",
                   __FUNCTION__, this->cycleCount);
}

void PsiActionSelectionAgent::executeAction(LanguageComprehension & languageTool,
                                            Procedure::ProcedureInterpreter & procedureInterpreter,
                                            const Procedure::ProcedureRepository & procedureRepository,
                                            Handle hActionExecutionLink)
{
    std::cout << "Currently executing Action: "
              << atomspace.atomAsString(this->current_action)
              << std::endl << std::endl;

#if HAVE_GUILE
    // Variables used by combo interpreter
    std::vector<combo::vertex> schemaArguments;

    // Get Action type
    Type actionType =
        atomspace.getType(atomspace.getOutgoing(hActionExecutionLink, 0));

    // Get Action name
    std::string actionName =
        atomspace.getName(atomspace.getOutgoing(hActionExecutionLink, 0));

    // Get scheme function name if any
    // @todo replace by opencog/execute call
    bool bSchemeFunction = false;
    size_t scm_prefix_index = actionName.find("scm:");

    if ( scm_prefix_index != std::string::npos ) {
        actionName = actionName.substr(scm_prefix_index+4);
        boost::trim(actionName);
        bSchemeFunction = true;
    }

    // Initialize scheme evaluator
    SchemeEval evaluator1(&atomspace);
    std::string scheme_expression, scheme_return_value;

    // If it is a SPEECH_ACT_SCHEMA_NODE, run the corresponding scheme function,
    // to create answers. The generated answers are stored in the format below,
    //
    // ReferenceLink
    //     UtteranceNode "utterance_sentences"
    //     ListLink
    //         SentenceNode ...
    //         ...
    // TODO: this is unnecessary, remove it later.
    // Why???
    if (actionType == SPEECH_ACT_SCHEMA_NODE) {
        scheme_expression = "( " + actionName + " )";

        // Run the speech act schema to generate answers
        scheme_return_value = evaluator1.eval(scheme_expression);

        if ( evaluator1.eval_error() ) {
            logger().error("PsiActionSelectionAgent::%s - Failed to execute '%s'",
                           __FUNCTION__,
                           scheme_expression.c_str());
        }

        logger().debug("PsiActionSelectionAgent::%s - "
                       "generate answers successfully by "
                       "SpeechActSchema: %s [cycle = %d]",
                       __FUNCTION__,
                       actionName.c_str(),
                       this->cycleCount);
    }
    // If it is a scheme function, call scheme evaluator
    else if ( bSchemeFunction ) {
        // Get arguments for the scheme function
        scheme_expression = actionName;

        // @todo replace by opencog/execute call
        if ( atomspace.getArity(hActionExecutionLink) == 2 ) {
            // Handle to ListLink containing arguments
            Handle hListLink = atomspace.getOutgoing(hActionExecutionLink, 1);

            // Process the arguments according to its type
            for (Handle hArgument : atomspace.getOutgoing(hListLink)) {
                Type argumentType = atomspace.getType(hArgument);
                if (argumentType == NUMBER_NODE) {
                    scheme_expression += " " + atomspace.getName(hArgument);
                }
                else {
                    scheme_expression += " \""
                        + atomspace.getName(hArgument) + "\"";
                }
            } // for
        } // if

        // TODO: A better approach is implementing 'answer_question'
        //       in 'unity_speech_act_schema.scm'.  We implement it
        //       here is because many previous c++ are required.
        if ( actionName == "answer_question" ) {
            languageTool.resolveLatestSentenceReference();
            languageTool.answerQuestion();
            logger().debug("PsiActionSelectionAgent::%s - "
                           "executed function: %s [cycle = %d]",
                           __FUNCTION__,
                           actionName.c_str(),
                           this->cycleCount);
        }
        else {
            scheme_expression = "( " + scheme_expression + " )";

            // Run scheme function
            scheme_return_value = evaluator1.eval(scheme_expression);

            if ( evaluator1.eval_error() ) {
                logger().error("PsiActionSelectionAgent::%s - "
                               "Failed to execute '%s'",
                               __FUNCTION__,
                               scheme_expression.c_str());
            }
            else {
                logger().debug("PsiActionSelectionAgent::%s - Successfully executed scheme function: %s [cycle = %d]",
                               __FUNCTION__,
                               scheme_expression.c_str(),
                               this->cycleCount);
            }
        } // if (actionName == "answer_question")
    }
    // If it is a combo function, call ProcedureInterpreter to execute
    // the function
    else  {
        // Get combo arguments for Action
        if ( atomspace.getArity(hActionExecutionLink) == 2 ) {
            // Handle to ListLink containing arguments
            Handle hListLink = atomspace.getOutgoing(hActionExecutionLink, 1);

            // Process the arguments according to its type
            for (Handle hArgument : atomspace.getOutgoing(hListLink)) {

                Type argumentType = atomspace.getType(hArgument);

                if (argumentType == NUMBER_NODE) {
                    string num_str = atomspace.getName(hArgument);
                    combo::contin_t num_c =
                        boost::lexical_cast<combo::contin_t>(num_str);
                    schemaArguments.push_back(combo::contin_t(num_c));
                }
                else {
                    schemaArguments.push_back(atomspace.getName(hArgument));
                }
            }// for
        }// if

        // Run the Procedure of the Action
        //
        // We will not check the state of the execution of the Action
        // here. Because it may take some time to finish it.  Instead,
        // we will check the result of the execution within 'run'
        // method during next "cognitive cycle".
        //
        // There are three kinds of results: success, fail and time
        // out (defined by 'PROCEDURE_EXECUTION_TIMEOUT')
        //
        // TODO: Before running the combo procedure, check the number
        // of arguments the procedure needed and it actually got
        //
        // Reference: "SchemaRunner.cc" line 264-286
        //
        const Procedure::GeneralProcedure & procedure = procedureRepository.get(actionName);

        this->currentSchemaId = procedureInterpreter.runProcedure(procedure, schemaArguments);

        logger().debug( "PsiActionSelectionAgent::%s - running action: %s [schemaId = %d, cycle = %d]",
                        __FUNCTION__,
                        procedure.getName().c_str(),
                        this->currentSchemaId,
                        this->cycleCount
                      );

    }
#endif // HAVE_GUILE

    // If the agent has something to say, generate a bunch of say
    // actions (one for each sentence node) which would be executed
    // from the next cognitive cycle
    {
        std::string sentenceNodeName, listerner, content;

        boost::regex expListener("TO\\s*:\\s*([^,\\s]*)[\\s|,]*");
        boost::regex expContent("CONTENT\\s*:\\s*(.*)");
        boost::smatch what;

        Handle hSpeakAction, hSpeakActionArgument;
        HandleSeq tempOutgoingSet;

        Handle hUtteranceSentencesList =
            AtomSpaceUtil::getReference(atomspace,
                                        atomspace.getHandle(UTTERANCE_NODE,
                                                            "utterance_sentences"
                                                           )
                                       );

        for (Handle hSentenceNode : atomspace.getOutgoing(hUtteranceSentencesList) ) {
            sentenceNodeName = atomspace.getName(hSentenceNode);

            // get listener and content of the sentence
            if ( boost::regex_search(sentenceNodeName, what, expListener) &&
                 what.size() == 2 && what[1].matched ) {
                listerner = what[1];
            }
            else
                listerner = "";

            if ( boost::regex_search(sentenceNodeName, what, expContent) &&
                 what.size() == 2 && what[1].matched ) {
                content = what[1];
            }
            else
                content = "";

            // skip the sentence with empty content (we should not get in there)
            if ( content=="" )
                continue;

            // create say action for the sentence and insert it into the action
            // list, which would be executed during next cognitive cycle
            tempOutgoingSet = { atomspace.addNode(SENTENCE_NODE, content),
                                atomspace.addNode(OBJECT_NODE, listerner) };
            hSpeakActionArgument = atomspace.addLink(LIST_LINK, tempOutgoingSet);

            tempOutgoingSet = { atomspace.addNode(GROUNDED_PREDICATE_NODE, "say"),
                                hSpeakActionArgument };
            hSpeakAction = atomspace.addLink(EXECUTION_LINK, tempOutgoingSet);

            this->temp_action_list.insert(this->temp_action_list.begin(),
                                          hSpeakAction);

            logger().debug("PsiActionSelectionAgent::%s - "
                           "generate say action: %s [cycle = %d]",
                           __FUNCTION__,
                           atomspace.atomAsString(hSpeakAction).c_str(),
                           this->cycleCount);

            std::cout << std::endl << "Generate say action "
                      << atomspace.atomAsString(hSpeakAction)
                      << " [cycle = " << this->cycleCount << "]" << std::endl;

        } // for

#if HAVE_GUILE
        // @todo this could be replaced by a GSN call (with opencog/execute)
        scheme_expression = "( reset_utterance_node \"utterance_sentences\" )";

        // Move sentences from UtteranceNode to DialogNode, then these sentences
        // will not be said again.
        scheme_return_value = evaluator1.eval(scheme_expression);

        if ( evaluator1.eval_error() ) {
            logger().error("PsiActionSelectionAgent::%s - Failed to execute '%s'",
                           __FUNCTION__,
                           scheme_expression.c_str());
        }
        else {
            logger().debug("PsiActionSelectionAgent::%s - "
                           "reset utterance node [cycle = %d]",
                           __FUNCTION__,
                           this->cycleCount);
        }
#endif // HAVE_GUILE
    }
}

void PsiActionSelectionAgent::run()
{
    this->cycleCount = _cogserver.getCycleCount();

    logger().debug("PsiActionSelectionAgent::%s - Executing run %d times",
                   __FUNCTION__, this->cycleCount);

    // Get Language Comprehension Tool
    LanguageComprehension & languageTool = oac.getPAI().getLanguageTool();

    // Get ProcedureInterpreter
    Procedure::ProcedureInterpreter & procedureInterpreter =
        oac.getProcedureInterpreter();

    // Get Procedure repository
    const Procedure::ProcedureRepository & procedureRepository =
        oac.getProcedureRepository();

    // Variables used by combo interpreter
    combo::vertex result; // combo::vertex is actually of type boost::variant <...>

    // Get pet
    Pet & pet = oac.getPet();

    // Get petId
    const std::string & petId = pet.getPetId();

    // Check if map info data is available
    if ( spaceServer().getLatestMapHandle() == Handle::UNDEFINED ) {
        logger().warn("PsiActionSelectionAgent::%s - "
                      "There is no map info available yet [cycle = %d]",
                      __FUNCTION__,
                      this->cycleCount);
        return;
    }

    // Check if the pet spatial info is already received
    Handle agent = AtomSpaceUtil::getAgentHandle(atomspace, petId);
    if (!spaceServer().getLatestMap().containsObject(agent)) {
        logger().warn("PsiActionSelectionAgent::%s - "
                      "Pet was not inserted in the space map yet [cycle = %d]",
                      __FUNCTION__,
                      this->cycleCount);
        return;
    }

    // Initialize the Mind Agent (demandGoalList etc)
    if ( !this->bInitialized )
        this->init();

    // Check the state of current running Action:
    //
    // If it success, fails, or is time out, update corresponding
    // information respectively, and continue processing.  Otherwise,
    // say the current Action is still running, do nothing and simply
    // returns.
    //
    if (this->currentSchemaId != 0) {

        logger().debug("PsiActionSelectionAgent::%s "
                       "currentSchemaId = %d [cycle = %d] ",
                       __FUNCTION__,
                       this->currentSchemaId,
                       this->cycleCount);
        bool schemaFailed =
            procedureInterpreter.isFailed(this->currentSchemaId);
        bool schemaComplete =
            procedureInterpreter.isFinished(this->currentSchemaId);
        // If the Action has completed, and was reported successful,
        // check the result
        if (schemaComplete && !schemaFailed) {

            logger().debug("PsiActionSelectionAgent::%s - "
                           "The Action %s is finished [SchemaId = %d, cycle = %d]",
                           __FUNCTION__,
                           atomspace.atomAsString(this->current_action).c_str(),
                           this->currentSchemaId,
                           this->cycleCount);

            std::cout << std::endl << "Action "
                      << atomspace.atomAsString(this->current_action)
                      <<" is done [SchemaId = "  << this->currentSchemaId
                      << ", cycle = " << this->cycleCount << "]" << std::endl;

            combo::vertex result =
                procedureInterpreter.getResult(this->currentSchemaId);

            // If check result: success
            if ((is_action_result(result)
                 && get_action(result) == combo::id::action_success)
                || (is_builtin(result)
                    && get_builtin(result) == combo::id::logical_true)) {

               logger().debug("PsiActionSelectionAgent::%s - "
                              "The Action %s succeeds [SchemaId = %d, cycle = %d]",
                              __FUNCTION__,
                              atomspace.atomAsString(this->current_action).c_str(),
                              this->currentSchemaId,
                              this->cycleCount);

               std::cout << "Action state: success" << std::endl << std::endl;
               // TODO: record the success and update the weight of
               // corresponding rule

            }
            // If check result: fail
            else if (is_action_result(result) || is_builtin(result)) {

                logger().debug("PsiActionSelectionAgent::%s - "
                               "The Action %s fails [SchemaId = %d, cycle = %d]",
                               __FUNCTION__,
                               atomspace.atomAsString(this->current_action).c_str(),
                               this->currentSchemaId,
                               this->cycleCount);

                std::cout << "action status: fail" << std::endl;

               // TODO: record the failure and update the weight of
               // corresponding rule
            }
            // If check result: unexpected result
            else {
                stringstream unexpected_result;
                unexpected_result << result;
                logger().warn("PsiActionSelectionAgent::%s - "
                              "Action procedure result should be "
                              "'built-in' or 'action result'. "
                              "Got '%s' [SchemaId = %d,  cycle = %d].",
                              __FUNCTION__,
                              unexpected_result.str().c_str(),
                              this->currentSchemaId,
                              this->cycleCount);

                std::cout << "action status: unexpected result" << std::endl;

                // TODO: record the failure and update the weight of
                // corresponding rule
            }
        }
        // If the Action fails
        else if ( schemaFailed ) {

            // TODO: How to judge whether an Action has failed?
            //
            //       Approach 1: Check if the Action has been
            //       finished, get the result, and then analyze the
            //       result
            //
            //       Approach 2: Check if the Action has failed
            //       directly via ProcedureInterpreter.isFailed method
            //
            //       We have implemented both approaches
            //       currently. However it seems one of them is
            //       surplus.
            //
            //       We should erase one of them, when we really
            //       understand the difference between both.
            //
            //       [By Zhennua Cai, on 2011-02-03]
            logger().debug("PsiActionSelectionAgent::%s - "
                           "The Action %s fails [SchemaId = %d, cycle = %d]",
                           __FUNCTION__,
                           atomspace.atomAsString(this->current_action).c_str(),
                           this->currentSchemaId,
                           this->cycleCount);

            std::cout << "action status: fail" << std::endl;

            // TODO: record the failure and update the weight of
            // corresponding rule

            // Troy: now that this action failed, the following action sequence
            // should be dropped.
            this->current_actions.clear();
            this->temp_action_list.clear();
        }
        // If the Action is time out
        else if (time(NULL) - this->timeStartCurrentAction
                 >  this->procedureExecutionTimeout) {

            logger().debug("PsiActionSelectionAgent::%s - "
                           "The Action %s is time out [SchemaId = %d, cycle = %d]",
                           __FUNCTION__,
                           atomspace.atomAsString(this->current_action).c_str(),
                           this->currentSchemaId,
                           this->cycleCount);

            // add 'actionFailed' predicates for timeout actions
            oac.getPAI().setPendingActionPlansFailed();

            // Stop the time out Action
            procedureInterpreter.stopProcedure(this->currentSchemaId);

            std::cout << "action status: timeout" << std::endl;

            // TODO: record the time out and update the weight of
            // corresponding rule

            // Troy: now that this action failed, the following action sequence
            // should be dropped.
            this->current_actions.clear();
            this->temp_action_list.clear();
        }
        // If the Action is still running and is not time out, simply returns
        else {
            logger().debug("PsiActionSelectionAgent::%s - "
                           "Current Action is still running. "
                           "[SchemaId = %d, cycle = %d]",
                           __FUNCTION__,
                           this->currentSchemaId, this->cycleCount);

            std::cout << "Current action is still running [SchemaId = "
                      << this->currentSchemaId << ", cycle = "
                      << this->cycleCount<<"] ... " << std::endl;

            return;
        }

        // Reset current schema id
        this->currentSchemaId = 0;

    }// if (this->currentSchemaId != 0)

#if HAVE_GUILE
    // If we've used up the current plan, do a new planning
    if ( this->temp_action_list.empty() && this->current_actions.empty() ) {
        // Initialize scheme evaluator
        SchemeEval evaluator1(&atomspace);
        std::string scheme_expression, scheme_return_value;

        // test: skip for some circles before beginning next planning
        // because there will be some results of the actions taken in
        // last plan are need to changed by other agent due
        static int count = 0;
        if (count++ < 4)
            return;
        count = 0;

        std::cout << "Doing planning ... " << std::endl;

        // @todo this could probably be reformulate the information
        // that do_planning updates are directly output, then call to
        // opencog/execute would return a Handle pointing the plan
        // information
        scheme_expression = "( do_planning )";

        // Run the Procedure that do planning
        scheme_return_value = evaluator1.eval(scheme_expression);

        if ( evaluator1.eval_error() ) {
            logger().error("PsiActionSelectionAgent::%s - Failed to execute '%s'",
                           __FUNCTION__,
                           scheme_expression.c_str());

            return;
        }

        // Try to get the plan stored in AtomSpace
        if ( !this->getPlan() ) {
            logger().warn("PsiActionSelectionAgent::%s - "
                          "'do_planning' can not find any suitable plan "
                          "for the selected demand goal [cycle = %d]",
                          __FUNCTION__,
                          this->cycleCount);

            std::cout << "'do_planning' can not find any suitable "
                      << "plan for the selected demand goal:"
                      << atomspace.atomAsString(this->plan_selected_demand_goal)
                      << ", [cycle = "
                      << this->cycleCount << "]." << std::endl;
            return;
        }

        this->stimulateAtoms();

        // std::cout << std::endl
        //           << "Done (do_planning), scheme_return_value(Handle): "
        //           << scheme_return_value;
        // Handle hPlanning = Handle( atol(scheme_return_value.c_str()) );
        // this->getPlan(hPlanning);

        // Print the plan to the screen
        this->printPlan();

        // Update the pet's previously/ currently Demand Goal
        PsiRuleUtil::setCurrentDemandGoal(atomspace,
                                          this->plan_selected_demand_goal);

        logger().debug("PsiActionSelectionAgent::%s - "
                       "do planning for the Demand Goal: %s [cycle = %d]",
                       __FUNCTION__,
                       atomspace.atomAsString(this->plan_selected_demand_goal).c_str(),
                       this->cycleCount);
    }
#endif // HAVE_GUILE

    // Get next action from current plan
    if ( !this->current_actions.empty() ) {
        this->current_action = this->current_actions.back();
        this->current_actions.pop_back();
    }
    else if ( !this->temp_action_list.empty() ) {
        this->getActions(this->temp_action_list.back(), this->current_actions);
        this->temp_action_list.pop_back();

        this->current_action = this->current_actions.back();
        this->current_actions.pop_back();
    }
    else {
        logger().debug("PsiActionSelectionAgent::%s - "
                       "Failed to get any actions from the planner. "
                       "Try planning next cycle [cycle = %d]",
                       __FUNCTION__, this->cycleCount);
        return;
    }

    // Execute current action
    this->executeAction(languageTool, procedureInterpreter, procedureRepository,
                        this->current_action);
    this->timeStartCurrentAction = time(NULL);

/**
    // TODO: The code snippets below show the idea of how the modulators
    // (or emotions) have impact on cognitive process, more specifically planning
    // here. We should implement the ideas in action_selection.scm later.

    // Figure out a plan for the selected Demand Goal
    int steps = 2000000;   // TODO: Emotional states shall have impact on steps, i.e., resource of cognitive process

    if (this->bPlanByPLN)
        this->planByPLN(server, selectedDemandGoal, this->psiPlanList, steps);
    else
        this->planByNaiveBreadthFirst(server, selectedDemandGoal,
                                      this->psiPlanList, steps);


    // Change the current Demand Goal randomly (controlled by the
    // modulator 'SelectionThreshold')
    float selectionThreshold =
        AtomSpaceUtil::getCurrentModulatorLevel(atomspace,
                                                SELECTION_THRESHOLD_MODULATOR_NAME);
    if ( randGen().randfloat() > selectionThreshold )
    {

        selectedDemandGoal = this->chooseRandomDemandGoal();

        if ( selectedDemandGoal == opencog::Handle::UNDEFINED ) {
            logger().warn("PsiActionSelectionAgent::%s - "
                          "Failed to randomly select a Demand Goal [cycle = %d]",
                          __FUNCTION__,
                          this->cycleCount);
            return;
        }

        Handle hCurrentDemandGoal, hReferenceLink;
        hCurrentDemandGoal =
            PsiRuleUtil::getCurrentDemandGoal(atomspace, hReferenceLink);

        if (selectedDemandGoal != hCurrentDemandGoal) {

            // Update the pet's previously/ currently Demand Goal
            PsiRuleUtil::setCurrentDemandGoal(atomspace, selectedDemandGoal);

            logger().debug("PsiActionSelectionAgent::%s - "
                           "Switch the Demand Goal to: %s [ cycle = %d ].",
                           __FUNCTION__,
                           atomspace.getName(atomspace.getOutgoing(selectedDemandGoal, 0)
                                             ).c_str(),
                           this->cycleCount);

            // Figure out a plan for the selected Demand Goal
            int steps = 2000000; // TODO: Emotional states shall have
                                 // impact on steps, i.e. resource of
                                 // cognitive process

            if (this->bPlanByPLN)
                this->planByPLN(server, selectedDemandGoal,
                                this->psiPlanList, steps);
            else
                this->planByNaiveBreadthFirst(server, selectedDemandGoal,
                                              this->psiPlanList, steps);
        }// if

    }// if
*/
}

