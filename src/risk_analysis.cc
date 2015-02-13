/// @file risk_analysis.cc
/// Implementation of risk analysis handler.
#include "risk_analysis.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/pointer_cast.hpp>

#if EMBED_SCHEMA
#include "schema.h"  // For static building.
#endif

#include "ccf_group.h"
#include "element.h"
#include "env.h"
#include "error.h"
#include "fault_tree.h"
#include "grapher.h"
#include "reporter.h"
#include "xml_parser.h"

namespace scram {

RiskAnalysis::RiskAnalysis(std::string config_file)
    : prob_requested_(false) {
  // Add valid gate types.
  gate_types_.insert("and");
  gate_types_.insert("or");
  gate_types_.insert("not");
  gate_types_.insert("nor");
  gate_types_.insert("nand");
  gate_types_.insert("xor");
  gate_types_.insert("null");
  gate_types_.insert("inhibit");
  gate_types_.insert("atleast");

  // Add valid primary event types.
  types_.insert("basic");
  types_.insert("undeveloped");
  types_.insert("house");
  types_.insert("conditional");

  // Add valid units.
  units_.insert(std::make_pair("bool", kBool));
  units_.insert(std::make_pair("int", kInt));
  units_.insert(std::make_pair("float", kFloat));
  units_.insert(std::make_pair("hours", kHours));
  units_.insert(std::make_pair("hours-1", kInverseHours));
  units_.insert(std::make_pair("years", kYears));
  units_.insert(std::make_pair("years-1", kInverseYears));
  units_.insert(std::make_pair("fit", kFit));
  units_.insert(std::make_pair("demands", kDemands));

  // Initialize the mission time with any value.
  mission_time_ = boost::shared_ptr<MissionTime>(new MissionTime());
}

void RiskAnalysis::ProcessInput(std::string xml_file) {
  std::vector<std::string> single;
  single.push_back(xml_file);
  RiskAnalysis::ProcessInputFiles(single);
}

void RiskAnalysis::ProcessInputFiles(
    const std::vector<std::string>& xml_files) {
  // Assume that setting are configured.
  mission_time_->mission_time(settings_.mission_time_);

  std::vector<std::string>::const_iterator it;
  for (it = xml_files.begin(); it != xml_files.end(); ++it) {
    try {
      RiskAnalysis::ProcessInputFile(*it);
    } catch (ValidationError& err) {
      err.msg("In file '" + *it + "', " + err.msg());
      throw err;
    }
  }

  if (!prob_requested_) {  // Only MCS analysis without probabilities.
    // Put all undefined events as primary events.
    boost::unordered_map<std::string, HouseEventPtr>::iterator it_h;
    for (it_h = tbd_house_events_.begin(); it_h != tbd_house_events_.end();
         ++it_h) {
      primary_events_.insert(std::make_pair(it_h->first, it_h->second));
    }

    boost::unordered_map<std::string, BasicEventPtr>::iterator it_b;
    for (it_b = tbd_basic_events_.begin(); it_b != tbd_basic_events_.end();
         ++it_b) {
      primary_events_.insert(std::make_pair(it_b->first, it_b->second));
      basic_events_.insert(std::make_pair(it_b->first, it_b->second));
    }

    boost::unordered_map<std::string, std::vector<GatePtr> >::iterator it_e;
    for (it_e = tbd_events_.begin(); it_e != tbd_events_.end(); ++it_e) {
      BasicEventPtr child(new BasicEvent(it_e->first));
      child->orig_id(tbd_orig_ids_.find(it_e->first)->second);
      primary_events_.insert(std::make_pair(it_e->first, child));
      basic_events_.insert(std::make_pair(it_e->first, child));
      std::vector<GatePtr>::iterator itvec = it_e->second.begin();
      for (; itvec != it_e->second.end(); ++itvec) {
        (*itvec)->AddChild(child);
        child->AddParent(*itvec);
      }
    }
  }

  // Check if the initialization is successful.
  RiskAnalysis::ValidateInitialization();

  // Perform setup for analysis using configurations from the input files.
  RiskAnalysis::SetupForAnalysis();
}

void RiskAnalysis::GraphingInstructions(std::string output) {
  std::ofstream of(output.c_str());
  if (!of.good()) {
    throw IOError(output +  " : Cannot write the graphing file.");
  }
  RiskAnalysis::GraphingInstructions(of);
}

void RiskAnalysis::Analyze() {
  // Set system mission time for all analysis.
  mission_time_->mission_time(settings_.mission_time_);

  std::map<std::string, FaultTreePtr>::iterator it;
  for (it = fault_trees_.begin(); it != fault_trees_.end(); ++it) {
    FaultTreeAnalysisPtr fta(new FaultTreeAnalysis(settings_.limit_order_));
    fta->Analyze(it->second);
    ftas_.push_back(fta);

    if (prob_requested_) {
      if (settings_.fta_type_ == "default") {
        ProbabilityAnalysisPtr pa(
            new ProbabilityAnalysis(settings_.approx_, settings_.num_sums_,
                                    settings_.cut_off_));
        pa->UpdateDatabase(it->second->basic_events());
        pa->Analyze(fta->min_cut_sets());
        prob_analyses_.push_back(pa);

      } else if (settings_.fta_type_ == "mc") {
        UncertaintyAnalysisPtr ua(new UncertaintyAnalysis(settings_.num_sums_,
                                                          settings_.cut_off_,
                                                          settings_.trials_));
        ua->UpdateDatabase(it->second->basic_events());
        ua->Analyze(fta->min_cut_sets());
        uncertainty_analyses_.push_back(ua);
      }
    }
  }
}

void RiskAnalysis::Report(std::string output) {
  std::ofstream of(output.c_str());
  if (!of.good()) {
    throw IOError(output +  " : Cannot write the output file.");
  }
  RiskAnalysis::Report(of);
}

void RiskAnalysis::ProcessInputFile(std::string xml_file) {
  std::ifstream file_stream(xml_file.c_str());
  if (!file_stream) {
    throw IOError("The file '" + xml_file + "' could not be loaded.");
  }

  std::stringstream stream;
  stream << file_stream.rdbuf();
  file_stream.close();

  boost::shared_ptr<XMLParser> parser(new XMLParser());
  parser->Init(stream);

  std::stringstream schema;
#if EMBED_SCHEMA
  schema << g_schema_content;
#else
  std::string schema_path = Env::rng_schema();
  std::ifstream schema_stream(schema_path.c_str());
  schema << schema_stream.rdbuf();
  schema_stream.close();
#endif
  parser->Validate(schema);

  xmlpp::Document* doc = parser->Document();
  const xmlpp::Node* root = doc->get_root_node();
  assert(root->get_name() == "opsa-mef");
  xmlpp::NodeSet roots_children = root->find("./*");
  xmlpp::NodeSet::iterator it_ch;
  for (it_ch = roots_children.begin();
       it_ch != roots_children.end(); ++it_ch) {
    const xmlpp::Element* element =
        dynamic_cast<const xmlpp::Element*>(*it_ch);
    assert(element);

    std::string name = element->get_name();
    if (name == "define-fault-tree") {
      RiskAnalysis::DefineFaultTree(element);

    } else if (name == "define-CCF-group") {
      RiskAnalysis::DefineCcfGroup(element);

    } else if (name == "model-data") {
      RiskAnalysis::ProcessModelData(element);

    } else {
      // Not yet capable of handling other analysis.
      throw(ValidationError("Cannot handle '" + name + "'"));
    }
  }
}

void RiskAnalysis::AttachLabelAndAttributes(
    const xmlpp::Element* element_node,
    const ElementPtr& element) {
  xmlpp::NodeSet labels = element_node->find("./*[name() = 'label']");
  if (!labels.empty()) {
    assert(labels.size() == 1);
    const xmlpp::Element* label =
        dynamic_cast<const xmlpp::Element*>(labels.front());
    assert(label);
    const xmlpp::TextNode* text = label->get_child_text();
    assert(text);
    element->label(text->get_content());
  }

  xmlpp::NodeSet attributes = element_node->find("./*[name() = 'attributes']");
  if (!attributes.empty()) {
    assert(attributes.size() == 1);  // Only on big element 'attributes'.
    const xmlpp::Element* attributes_element =
        dynamic_cast<const xmlpp::Element*>(attributes.front());
    xmlpp::NodeSet attribute_list =
        attributes_element->find("./*[name() = 'attribute']");
    xmlpp::NodeSet::iterator it;
    for (it = attribute_list.begin(); it != attribute_list.end(); ++it) {
      const xmlpp::Element* attribute =
          dynamic_cast<const xmlpp::Element*>(*it);
      Attribute attr;
      attr.name = attribute->get_attribute_value("name");
      attr.value = attribute->get_attribute_value("value");
      attr.type = attribute->get_attribute_value("type");
      element->AddAttribute(attr);
    }
  }
}

void RiskAnalysis::DefineGate(const xmlpp::Element* gate_node,
                              const FaultTreePtr& ft) {
  // Only one child element is expected, which is formulae.
  std::string orig_id = gate_node->get_attribute_value("name");
  boost::trim(orig_id);
  std::string id = orig_id;
  boost::to_lower(id);

  /// @todo This should take private/public roles into account.
  if (gates_.count(id)) {
    // Doubly defined gate.
    std::stringstream msg;
    msg << "Line " << gate_node->get_line() << ":\n";
    msg << orig_id << " gate is doubly defined.";
    throw ValidationError(msg.str());
  }

  // Detect name clashes.
  if (primary_events_.count(id) || tbd_basic_events_.count(id) ||
      tbd_house_events_.count(id)) {
    std::stringstream msg;
    msg << "Line " << gate_node->get_line() << ":\n";
    msg << "The id " << orig_id << " is already assigned to a primary event.";
    throw ValidationError(msg.str());
  }

  xmlpp::NodeSet gates =
      gate_node->find("./*[name() != 'attributes' and name() != 'label']");
  // Assumes that there are no attributes and labels.
  assert(gates.size() == 1);
  // Check if the gate type is supported.
  const xmlpp::Node* gate_type = gates.front();
  std::string type = gate_type->get_name();
  if (!gate_types_.count(type)) {
    std::stringstream msg;
    msg << "Line " << gate_type->get_line() << ":\n";
    msg << "Invalid input arguments. '" << orig_id
        << "' gate formulae is not supported.";
    throw ValidationError(msg.str());
  }

  int vote_number = -1;  // For atleast/vote gates.
  if (type == "atleast") {
    const xmlpp::Element* gate =
        dynamic_cast<const xmlpp::Element*>(gate_type);
    std::string min_num = gate->get_attribute_value("min");
    boost::trim(min_num);
    vote_number = boost::lexical_cast<int>(min_num);
  }

  GatePtr i_event(new Gate(id));
  i_event->orig_id(orig_id);

  // Update to be defined events.
  if (tbd_gates_.count(id)) {
    i_event = tbd_gates_.find(id)->second;
    tbd_gates_.erase(id);
  } else {
    RiskAnalysis::UpdateIfLateEvent(i_event);
  }

  assert(!gates_.count(id));
  gates_.insert(std::make_pair(id, i_event));

  RiskAnalysis::AttachLabelAndAttributes(gate_node, i_event);

  ft->AddGate(i_event);

  i_event->type(type);  // Setting the gate type.
  if (type == "atleast") i_event->vote_number(vote_number);

  // Process children formula of this gate.
  RiskAnalysis::ProcessFormula(i_event, gate_type->find("./*"));
}

void RiskAnalysis::ProcessFormula(const GatePtr& gate,
                                  const xmlpp::NodeSet& events) {
  std::set<std::string> children_id;  // To detect repeated children.
  xmlpp::NodeSet::const_iterator it;
  for (it = events.begin(); it != events.end(); ++it) {
    const xmlpp::Element* event = dynamic_cast<const xmlpp::Element*>(*it);
    assert(event);
    std::string orig_id = event->get_attribute_value("name");
    boost::trim(orig_id);
    std::string id = orig_id;
    boost::to_lower(id);

    if (children_id.count(id)) {
      std::stringstream msg;
      msg << "Line " << event->get_line() << ":\n";
      msg << "Detected a repeated child " << orig_id;
      throw ValidationError(msg.str());
    } else {
      children_id.insert(id);
    }

    std::string name = event->get_name();
    assert(name == "event" || name == "gate" || name == "basic-event" ||
           name == "house-event");
    // This is for a case "<event name="id" type="type"/>".
    std::string type = event->get_attribute_value("type");
    boost::trim(type);
    if (type != "") {
      assert(type == "gate" || type == "basic-event" ||
             type == "house-event");
      name = type;  // Event type is defined.
    }

    EventPtr child(new Event(id));
    child->orig_id(orig_id);
    if (name == "event") {  // Undefined type yet.
      if (!RiskAnalysis::ProcessFormulaEvent(gate, child)) continue;

    } else if (name == "gate") {
      RiskAnalysis::ProcessFormulaGate(event, gate, child);

    } else if (name == "basic-event") {
      RiskAnalysis::ProcessFormulaBasicEvent(event, gate, child);

    } else if (name == "house-event") {
      RiskAnalysis::ProcessFormulaHouseEvent(event, gate, child);
    }

    gate->AddChild(child);
    child->AddParent(gate);
  }
}

bool RiskAnalysis::ProcessFormulaEvent(const GatePtr& gate,
                                       EventPtr& child) {
  std::string id = child->id();
  std::string orig_id = child->orig_id();
  if (primary_events_.count(id)) {
    child = primary_events_.find(id)->second;

  } else if (gates_.count(id)) {
    child = gates_.find(id)->second;

  } else if (tbd_gates_.count(id)) {
    child = tbd_gates_.find(id)->second;

  } else if (tbd_basic_events_.count(id)) {
    child = tbd_basic_events_.find(id)->second;

  } else if (tbd_house_events_.count(id)) {
    child = tbd_house_events_.find(id)->second;

  } else {
    if (tbd_events_.count(id)) {
      tbd_events_.find(id)->second.push_back(gate);
    } else {
      std::vector<GatePtr> parents;
      parents.push_back(gate);
      tbd_events_.insert(std::make_pair(id, parents));
      tbd_orig_ids_.insert(std::make_pair(id, orig_id));
    }
    return false;
  }
  return true;
}

void RiskAnalysis::ProcessFormulaBasicEvent(const xmlpp::Element* event,
                                            const GatePtr& gate,
                                            EventPtr& child) {
  std::string id = child->id();
  std::string orig_id = child->orig_id();
  if (gates_.count(id) || tbd_gates_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event->get_line() << ":\n";
    msg << "The id " << orig_id << " is already assigned to a gate.";
    throw ValidationError(msg.str());
  }
  if (tbd_house_events_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event->get_line() << ":\n";
    msg << "The id " << orig_id << " is already used by a house event.";
    throw ValidationError(msg.str());
  }
  if (primary_events_.count(id)) {
    child = primary_events_.find(id)->second;
    if (boost::dynamic_pointer_cast<BasicEvent>(child) == 0) {
      std::stringstream msg;
      msg << "Line " << event->get_line() << ":\n";
      msg << "The id " << orig_id << " is already assigned to a house event.";
      throw ValidationError(msg.str());
    }
  } else if (tbd_basic_events_.count(id)) {
    child = tbd_basic_events_.find(id)->second;
  } else {
    child = BasicEventPtr(new BasicEvent(id));
    child->orig_id(orig_id);
    tbd_basic_events_.insert(
        std::make_pair(id, boost::dynamic_pointer_cast <BasicEvent>(child)));
    RiskAnalysis::UpdateIfLateEvent(child);
  }
}

void RiskAnalysis::ProcessFormulaHouseEvent(const xmlpp::Element* event,
                                            const GatePtr& gate,
                                            EventPtr& child) {
  std::string id = child->id();
  std::string orig_id = child->orig_id();
  if (gates_.count(id) || tbd_gates_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event->get_line() << ":\n";
    msg << "The id " << orig_id << " is already assigned to a gate.";
    throw ValidationError(msg.str());
  }
  if (tbd_basic_events_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event->get_line() << ":\n";
    msg << "The id " << orig_id << " is already used by a basic event.";
    throw ValidationError(msg.str());
  }
  if (primary_events_.count(id)) {
    child = primary_events_.find(id)->second;
    if (boost::dynamic_pointer_cast<HouseEvent>(child) == 0) {
      std::stringstream msg;
      msg << "Line " << event->get_line() << ":\n";
      msg << "The id " << orig_id << " is already assigned to a basic event.";
      throw ValidationError(msg.str());
    }
  } else if (tbd_house_events_.count(id)) {
    child = tbd_house_events_.find(id)->second;
  } else {
    child = HouseEventPtr(new HouseEvent(id));
    child->orig_id(orig_id);
    tbd_house_events_.insert(
        std::make_pair(id, boost::dynamic_pointer_cast<HouseEvent>(child)));
    RiskAnalysis::UpdateIfLateEvent(child);
  }
}

void RiskAnalysis::ProcessFormulaGate(const xmlpp::Element* event,
                                      const GatePtr& gate,
                                      EventPtr& child) {
  std::string id = child->id();
  std::string orig_id = child->orig_id();
  if (primary_events_.count(id) || tbd_basic_events_.count(id) ||
      tbd_house_events_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event->get_line() << ":\n";
    msg << "The id " << orig_id << " is already assigned to a primary event.";
    throw ValidationError(msg.str());
  }
  if (gates_.count(id)) {
    child = gates_.find(id)->second;
  } else if (tbd_gates_.count(id)) {
    child = tbd_gates_.find(id)->second;
  } else {
    child = GatePtr(new Gate(id));
    child->orig_id(orig_id);
    tbd_gates_.insert(
        std::make_pair(id, boost::dynamic_pointer_cast<Gate>(child)));
    RiskAnalysis::UpdateIfLateEvent(child);
  }
}

void RiskAnalysis::DefineBasicEvent(const xmlpp::Element* event_node) {
  xmlpp::NodeSet expressions =
     event_node->find("./*[name() != 'attributes' and name() != 'label']");

  BasicEventPtr basic_event;
  RiskAnalysis::GetBasicEvent(event_node, basic_event);

  if (!expressions.empty()) {
    const xmlpp::Element* expr_node =
        dynamic_cast<const xmlpp::Element*>(expressions.back());
    assert(expr_node);

    ExpressionPtr expression;
    RiskAnalysis::GetExpression(expr_node, expression);
    basic_event->expression(expression);
  } else {
    std::stringstream msg;
    msg << "Line " << event_node->get_line() << ":\n";
    msg << "The " << basic_event->orig_id()
        << " basic event does not have an expression.";
    throw ValidationError(msg.str());
  }

  RiskAnalysis::AttachLabelAndAttributes(event_node, basic_event);
}

void RiskAnalysis::GetBasicEvent(const xmlpp::Element* event_node,
                                 BasicEventPtr& basic_event) {
  std::string orig_id = event_node->get_attribute_value("name");
  boost::trim(orig_id);
  std::string id = orig_id;
  boost::to_lower(id);
  // Detect name clashes.
  if (gates_.count(id) || tbd_gates_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event_node->get_line() << ":\n";
    msg << "The id " << orig_id << " is already assigned to a gate.";
    throw ValidationError(msg.str());
  }
  if (primary_events_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event_node->get_line() << ":\n";
    msg << "The id " << orig_id << " is doubly defined.";
    throw ValidationError(msg.str());
  }
  if (tbd_house_events_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event_node->get_line() << ":\n";
    msg << "The id " << orig_id << " is already used by a house event.";
    throw ValidationError(msg.str());
  }

  if (tbd_basic_events_.count(id)) {
    basic_event = tbd_basic_events_.find(id)->second;
    primary_events_.insert(std::make_pair(id, basic_event));
    basic_events_.insert(std::make_pair(id, basic_event));
    tbd_basic_events_.erase(id);

  } else {
    basic_event = BasicEventPtr(new BasicEvent(id));
    basic_event->orig_id(orig_id);
    primary_events_.insert(std::make_pair(id, basic_event));
    basic_events_.insert(std::make_pair(id, basic_event));
    RiskAnalysis::UpdateIfLateEvent(basic_event);
  }
}

void RiskAnalysis::DefineHouseEvent(const xmlpp::Element* event_node) {
  std::string orig_id = event_node->get_attribute_value("name");
  boost::trim(orig_id);
  std::string id = orig_id;
  boost::to_lower(id);
  // Detect name clashes.
  if (gates_.count(id) || tbd_gates_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event_node->get_line() << ":\n";
    msg << "The id " << orig_id << " is already assigned to a gate.";
    throw ValidationError(msg.str());
  }
  if (primary_events_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event_node->get_line() << ":\n";
    msg << "The id " << orig_id << " is doubly defined.";
    throw ValidationError(msg.str());
  }
  if (tbd_basic_events_.count(id)) {
    std::stringstream msg;
    msg << "Line " << event_node->get_line() << ":\n";
    msg << "The id " << orig_id << " is already used by a basic event.";
    throw ValidationError(msg.str());
  }
  // Only boolean for now.
  xmlpp::NodeSet expression = event_node->find("./*[name() = 'constant']");

  if (expression.empty()) {
    std::stringstream msg;
    msg << "Line " << event_node->get_line() << ":\n";
    msg << "The " << orig_id
        << " house event does not have a Boolean constant expression.";
    throw ValidationError(msg.str());
  }

  assert(expression.size() == 1);

  const xmlpp::Element* constant =
      dynamic_cast<const xmlpp::Element*>(expression.front());
  if (!constant) assert(false);

  std::string val = constant->get_attribute_value("value");
  boost::trim(val);
  assert(val == "true" || val == "false");

  bool state = (val == "true") ? true : false;

  HouseEventPtr house_event;

  if (tbd_house_events_.count(id)) {
    house_event = tbd_house_events_.find(id)->second;
    house_event->state(state);
    primary_events_.insert(std::make_pair(id, house_event));
    tbd_house_events_.erase(id);

  } else {
    house_event = HouseEventPtr(new HouseEvent(id));
    house_event->orig_id(orig_id);
    house_event->state(state);
    primary_events_.insert(std::make_pair(id, house_event));
    RiskAnalysis::UpdateIfLateEvent(house_event);
  }

  RiskAnalysis::AttachLabelAndAttributes(event_node, house_event);
}

void RiskAnalysis::DefineParameter(const xmlpp::Element* param_node) {
  std::string name = param_node->get_attribute_value("name");
  boost::trim(name);
  // Detect case sensitive name clashes.
  if (parameters_.count(name)) {
    std::stringstream msg;
    msg << "Line " << param_node->get_line() << ":\n";
    msg << "The " << name << " parameter is doubly defined.";
    throw ValidationError(msg.str());
  }

  ParameterPtr parameter;

  if (tbd_parameters_.count(name)) {
    parameter = tbd_parameters_.find(name)->second;
    parameters_.insert(std::make_pair(name, parameter));
    tbd_parameters_.erase(name);

  } else {
    parameter = ParameterPtr(new Parameter(name));
    parameters_.insert(std::make_pair(name, parameter));
  }

  // Attach units.
  std::string unit = param_node->get_attribute_value("unit");
  if (unit != "") {
    assert(units_.count(unit));
    /// @todo Check for parameter unit clash or double definition.
    parameter->unit(units_.find(unit)->second);
  }
  // Assuming that expression is the last child of the parameter definition.
  xmlpp::NodeSet expressions =
      param_node->find("./*[name() != 'attributes' and name() != 'label']");
  assert(expressions.size() == 1);
  const xmlpp::Element* expr_node =
      dynamic_cast<const xmlpp::Element*>(expressions.back());
  assert(expr_node);
  ExpressionPtr expression;
  RiskAnalysis::GetExpression(expr_node, expression);

  parameter->expression(expression);
  RiskAnalysis::AttachLabelAndAttributes(param_node, parameter);
}

void RiskAnalysis::GetExpression(const xmlpp::Element* expr_element,
                                 ExpressionPtr& expression) {
  using scram::RiskAnalysis;
  assert(expr_element);
  if (GetConstantExpression(expr_element, expression)) {
  } else if (GetParameterExpression(expr_element, expression)) {
  } else if (GetDeviateExpression(expr_element, expression)) {
  } else {
    std::stringstream msg;
    msg << "Line " << expr_element->get_line() << ":\n";
    msg << "Unsupported expression: " << expr_element->get_name();
    throw ValidationError(msg.str());
  }

  expressions_.insert(expression);
}

bool RiskAnalysis::GetConstantExpression(const xmlpp::Element* expr_element,
                                         ExpressionPtr& expression) {
  typedef boost::shared_ptr<ConstantExpression> ConstantExpressionPtr;
  assert(expr_element);
  std::string expr_name = expr_element->get_name();
  if (expr_name == "float" || expr_name == "int") {
    std::string val = expr_element->get_attribute_value("value");
    boost::trim(val);
    double num = boost::lexical_cast<double>(val);
    expression = ConstantExpressionPtr(new ConstantExpression(num));

  } else if (expr_name == "bool") {
    std::string val = expr_element->get_attribute_value("value");
    boost::trim(val);
    bool state = (val == "true") ? true : false;
    expression = ConstantExpressionPtr(new ConstantExpression(state));
  } else {
    return false;
  }
  return true;
}

bool RiskAnalysis::GetParameterExpression(const xmlpp::Element* expr_element,
                                          ExpressionPtr& expression) {
  assert(expr_element);
  std::string expr_name = expr_element->get_name();
  if (expr_name == "parameter") {
    std::string name = expr_element->get_attribute_value("name");
    /// @todo check for possible unit clashes.
    if (parameters_.count(name)) {
      expression = parameters_.find(name)->second;

    } else if (tbd_parameters_.count(name)) {
      expression = tbd_parameters_.find(name)->second;

    } else {
      ParameterPtr param(new Parameter(name));
      tbd_parameters_.insert(std::make_pair(name, param));
      expression = param;
    }
  } else if (expr_name == "system-mission-time") {
    /// @todo check for possible unit clashes.
    expression = mission_time_;

  } else {
    return false;
  }
  return true;
}

bool RiskAnalysis::GetDeviateExpression(const xmlpp::Element* expr_element,
                                        ExpressionPtr& expression) {
  assert(expr_element);
  std::string expr_name = expr_element->get_name();
  if (expr_name == "uniform-deviate") {
    assert(expr_element->find("./*").size() == 2);
    const xmlpp::Element* element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[0]);
    assert(element);
    ExpressionPtr min;
    GetExpression(element, min);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[1]);
    assert(element);
    ExpressionPtr max;
    GetExpression(element, max);

    expression = boost::shared_ptr<UniformDeviate>(
        new UniformDeviate(min, max));

  } else if (expr_name == "normal-deviate") {
    assert(expr_element->find("./*").size() == 2);
    const xmlpp::Element* element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[0]);
    assert(element);
    ExpressionPtr mean;
    GetExpression(element, mean);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[1]);
    assert(element);
    ExpressionPtr sigma;
    GetExpression(element, sigma);

    expression = boost::shared_ptr<NormalDeviate>(
        new NormalDeviate(mean, sigma));

  } else if (expr_name == "lognormal-deviate") {
    assert(expr_element->find("./*").size() == 3);
    const xmlpp::Element* element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[0]);
    assert(element);
    ExpressionPtr mean;
    GetExpression(element, mean);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[1]);
    assert(element);
    ExpressionPtr ef;
    GetExpression(element, ef);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[2]);
    assert(element);
    ExpressionPtr level;
    GetExpression(element, level);

    expression = boost::shared_ptr<LogNormalDeviate>(
        new LogNormalDeviate(mean, ef, level));

  } else if (expr_name == "gamma-deviate") {
    assert(expr_element->find("./*").size() == 2);
    const xmlpp::Element* element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[0]);
    assert(element);
    ExpressionPtr k;
    GetExpression(element, k);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[1]);
    assert(element);
    ExpressionPtr theta;
    GetExpression(element, theta);

    expression = boost::shared_ptr<GammaDeviate>(new GammaDeviate(k, theta));

  } else if (expr_name == "beta-deviate") {
    assert(expr_element->find("./*").size() == 2);
    const xmlpp::Element* element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[0]);
    assert(element);
    ExpressionPtr alpha;
    GetExpression(element, alpha);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[1]);
    assert(element);
    ExpressionPtr beta;
    GetExpression(element, beta);

    expression = boost::shared_ptr<BetaDeviate>(new BetaDeviate(alpha, beta));

  } else if (expr_name == "histogram") {
    std::vector<ExpressionPtr> boundaries;
    std::vector<ExpressionPtr> weights;
    xmlpp::NodeSet bins = expr_element->find("./*");
    xmlpp::NodeSet::iterator it;
    for (it = bins.begin(); it != bins.end(); ++it) {
      const xmlpp::Element* el = dynamic_cast<const xmlpp::Element*>(*it);
      assert(el->find("./*").size() == 2);
      const xmlpp::Element* element =
          dynamic_cast<const xmlpp::Element*>(el->find("./*")[0]);
      assert(element);
      ExpressionPtr bound;
      GetExpression(element, bound);
      boundaries.push_back(bound);

      element =
          dynamic_cast<const xmlpp::Element*>(el->find("./*")[1]);
      assert(element);
      ExpressionPtr weight;
      GetExpression(element, weight);
      weights.push_back(weight);
    }
    expression = boost::shared_ptr<Histogram>(
        new Histogram(boundaries, weights));

  } else if (expr_name == "exponential") {
    assert(expr_element->find("./*").size() == 2);
    const xmlpp::Element* element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[0]);
    assert(element);
    ExpressionPtr lambda;
    GetExpression(element, lambda);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[1]);
    assert(element);
    ExpressionPtr time;
    GetExpression(element, time);

    expression = boost::shared_ptr<ExponentialExpression>(
        new ExponentialExpression(lambda, time));

  } else if (expr_name == "GLM") {
    assert(expr_element->find("./*").size() == 4);
    const xmlpp::Element* element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[0]);
    assert(element);
    ExpressionPtr gamma;
    GetExpression(element, gamma);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[1]);
    assert(element);
    ExpressionPtr lambda;
    GetExpression(element, lambda);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[2]);
    assert(element);
    ExpressionPtr mu;
    GetExpression(element, mu);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[3]);
    assert(element);
    ExpressionPtr time;
    GetExpression(element, time);

    expression = boost::shared_ptr<GlmExpression>(
        new GlmExpression(gamma, lambda, mu, time));

  } else if (expr_name == "Weibull") {
    assert(expr_element->find("./*").size() == 4);
    const xmlpp::Element* element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[0]);
    assert(element);
    ExpressionPtr alpha;
    GetExpression(element, alpha);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[1]);
    assert(element);
    ExpressionPtr beta;
    GetExpression(element, beta);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[2]);
    assert(element);
    ExpressionPtr t0;
    GetExpression(element, t0);

    element =
        dynamic_cast<const xmlpp::Element*>(expr_element->find("./*")[3]);
    assert(element);
    ExpressionPtr time;
    GetExpression(element, time);

    expression = boost::shared_ptr<WeibullExpression>(
        new WeibullExpression(alpha, beta, t0, time));
  } else {
    return false;
  }
  return true;
}

bool RiskAnalysis::UpdateIfLateEvent(const EventPtr& event) {
  std::string id = event->id();
  if (tbd_events_.count(id)) {
    std::vector<GatePtr>::iterator it;
    for (it = tbd_events_.find(id)->second.begin();
         it != tbd_events_.find(id)->second.end(); ++it) {
      (*it)->AddChild(event);
      event->AddParent(*it);
    }
    tbd_events_.erase(id);
    tbd_orig_ids_.erase(id);
    return true;
  } else {
    return false;
  }
}

void RiskAnalysis::DefineFaultTree(const xmlpp::Element* ft_node) {
  std::string name = ft_node->get_attribute_value("name");
  std::string id = name;
  boost::to_lower(id);

  if (fault_trees_.count(id)) {
    std::stringstream msg;
    msg << "Line " << ft_node->get_line() << ":\n";
    msg << "The fault tree " << name
        << " is already defined.";
    throw ValidationError(msg.str());
  }

  FaultTreePtr fault_tree = FaultTreePtr(new FaultTree(name));
  fault_trees_.insert(std::make_pair(id, fault_tree));

  RiskAnalysis::AttachLabelAndAttributes(ft_node, fault_tree);

  xmlpp::NodeSet children = ft_node->find("./*");
  xmlpp::NodeSet::iterator it;
  for (it = children.begin(); it != children.end(); ++it) {
    const xmlpp::Element* element = dynamic_cast<const xmlpp::Element*>(*it);

    assert(element);
    std::string name = element->get_name();

    if (!prob_requested_ &&
        (name == "define-basic-event" || name == "define-house-event")) {
      prob_requested_ = true;
    }

    if (name == "define-gate") {
      RiskAnalysis::DefineGate(element, fault_tree);

    } else if (name == "define-basic-event") {
      RiskAnalysis::DefineBasicEvent(element);

    } else if (name == "define-house-event") {
      RiskAnalysis::DefineHouseEvent(element);

    } else if (name == "define-parameter") {
      RiskAnalysis::DefineParameter(element);

    } else if (name == "define-CCF-group") {
      RiskAnalysis::DefineCcfGroup(element);
    }
  }
}

void RiskAnalysis::ProcessModelData(const xmlpp::Element* model_data) {
  prob_requested_ = true;

  xmlpp::NodeSet children = model_data->find("./*");
  xmlpp::NodeSet::iterator it;
  for (it = children.begin(); it != children.end(); ++it) {
    const xmlpp::Element* element = dynamic_cast<const xmlpp::Element*>(*it);
    assert(element);

    std::string name = element->get_name();

    if (name == "define-basic-event") {
      RiskAnalysis::DefineBasicEvent(element);

    } else if (name == "define-house-event") {
      RiskAnalysis::DefineHouseEvent(element);

    } else if (name == "define-parameter") {
      RiskAnalysis::DefineParameter(element);
    }
  }
}

void RiskAnalysis::ValidateInitialization() {
  // Validation of essential members of analysis in the first layer.
  RiskAnalysis::CheckFirstLayer();

  // Validation of fault trees.
  RiskAnalysis::CheckSecondLayer();

  // Gather orphan primary events for warning.
  boost::unordered_map<std::string, PrimaryEventPtr>::iterator it_p;
  for (it_p = primary_events_.begin(); it_p != primary_events_.end(); ++it_p) {
    try {
      it_p->second->parents();
    } catch (LogicError& err) {
      orphan_primary_events_.insert(it_p->second);
    }
  }
}

void RiskAnalysis::DefineCcfGroup(const xmlpp::Element* ccf_node) {
  std::string name = ccf_node->get_attribute_value("name");
  std::string id = name;
  boost::to_lower(id);

  if (ccf_groups_.count(id)) {
    std::stringstream msg;
    msg << "Line " << ccf_node->get_line() << ":\n";
    msg << "The CCF group " << name << " is already defined.";
    throw ValidationError(msg.str());
  }
  std::string model = ccf_node->get_attribute_value("model");
  assert(model == "beta-factor" || model == "alpha-factor" ||
         model == "MGL" || model == "phi-factor");

  CcfGroupPtr ccf_group;
  if (model == "beta-factor") {
    ccf_group = CcfGroupPtr(new BetaFactorModel(name));

  } else if (model == "MGL") {
    ccf_group = CcfGroupPtr(new MglModel(name));

  } else if (model == "alpha-factor") {
    ccf_group = CcfGroupPtr(new AlphaFactorModel(name));

  } else if (model == "phi-factor") {
    ccf_group = CcfGroupPtr(new PhiFactorModel(name));
  }

  ccf_groups_.insert(std::make_pair(id, ccf_group));

  prob_requested_ = true;

  RiskAnalysis::AttachLabelAndAttributes(ccf_node, ccf_group);

  xmlpp::NodeSet children = ccf_node->find("./*");
  xmlpp::NodeSet::iterator it;
  for (it = children.begin(); it != children.end(); ++it) {
    const xmlpp::Element* element = dynamic_cast<const xmlpp::Element*>(*it);

    assert(element);
    std::string name = element->get_name();

    if (name == "members") {
      RiskAnalysis::ProcessCcfMembers(element, ccf_group);

    } else if (name == "distribution") {
      assert(element->find("./*").size() == 1);
      const xmlpp::Element* expr_node =
          dynamic_cast<const xmlpp::Element*>(*element->find("./*").begin());
      ExpressionPtr expression;
      RiskAnalysis::GetExpression(expr_node, expression);
      ccf_group->AddDistribution(expression);

    } else if (name == "factor") {
      RiskAnalysis::DefineCcfFactor(element, ccf_group);

    } else if (name == "factors") {
      RiskAnalysis::ProcessCcfFactors(element, ccf_group);
    }
  }
}

void RiskAnalysis::ProcessCcfMembers(const xmlpp::Element* members_node,
                                     const CcfGroupPtr& ccf_group) {
  xmlpp::NodeSet children = members_node->find("./*");
  assert(!children.empty());
  xmlpp::NodeSet::iterator it;
  for (it = children.begin(); it != children.end(); ++it) {
    const xmlpp::Element* event_node =
        dynamic_cast<const xmlpp::Element*>(*it);
    assert(event_node);
    assert("basic-event" == event_node->get_name());

    BasicEventPtr basic_event;
    RiskAnalysis::GetBasicEvent(event_node, basic_event);

    ccf_group->AddMember(basic_event);
  }
}

void RiskAnalysis::ProcessCcfFactors(const xmlpp::Element* factors_node,
                                     const CcfGroupPtr& ccf_group) {
  xmlpp::NodeSet children = factors_node->find("./*");
  assert(!children.empty());
  // To keep track of CCF group factor levels.
  xmlpp::NodeSet::iterator it;
  for (it = children.begin(); it != children.end(); ++it) {
    const xmlpp::Element* factor_node =
        dynamic_cast<const xmlpp::Element*>(*it);
    assert(factor_node);
    RiskAnalysis::DefineCcfFactor(factor_node, ccf_group);
  }
}

void RiskAnalysis::DefineCcfFactor(const xmlpp::Element* factor_node,
                                   const CcfGroupPtr& ccf_group) {
  // Checking the level for one factor input.
  std::string level = factor_node->get_attribute_value("level");
  boost::trim(level);
  if (level == "") {
    std::stringstream msg;
    msg << "Line " << factor_node->get_line() << ":\n";
    msg << "The CCF group factor level number is not provided.";
    throw ValidationError(msg.str());
  }
  int level_num = boost::lexical_cast<int>(level);
  assert(factor_node->find("./*").size() == 1);
  const xmlpp::Element* expr_node =
      dynamic_cast<const xmlpp::Element*>(*factor_node->find("./*").begin());
  ExpressionPtr expression;
  RiskAnalysis::GetExpression(expr_node, expression);
  try {
    ccf_group->AddFactor(expression, level_num);
  } catch (ValidationError& err) {
    std::stringstream msg;
    msg << "Line " << factor_node->get_line() << ":\n";
    msg << err.msg();
    throw ValidationError(msg.str());
  }
}

void RiskAnalysis::CheckFirstLayer() {
  std::stringstream error_messages;
  // Checking uninitialized gates.
  if (!tbd_gates_.empty()) {
    error_messages << "Undefined gates:\n";
    boost::unordered_map<std::string, GatePtr>::iterator it;
    for (it = tbd_gates_.begin(); it != tbd_gates_.end(); ++it) {
      error_messages << it->second->orig_id() << "\n";
    }
  }

  // Check if all initialized gates have the right number of children.
  std::string bad_gates = RiskAnalysis::CheckAllGates();
  if (!bad_gates.empty()) {
    error_messages << "\nThere are problems with the initialized gates:\n"
                   << bad_gates;
  }

  // Check if all events are initialized.
  if (prob_requested_) {
    // Check if some members are missing definitions.
    error_messages << RiskAnalysis::CheckMissingEvents();
    error_messages << RiskAnalysis::CheckMissingParameters();
  }

  if (!error_messages.str().empty()) {
    throw ValidationError(error_messages.str()); }

  RiskAnalysis::ValidateExpressions();
}

void RiskAnalysis::CheckSecondLayer() {
  if (!fault_trees_.empty()) {
    std::map<std::string, FaultTreePtr>::iterator it;
    for (it = fault_trees_.begin(); it != fault_trees_.end(); ++it) {
      it->second->Validate();
    }
  }

  if (!ccf_groups_.empty()) {
    std::map<std::string, CcfGroupPtr>::iterator it;
    for (it = ccf_groups_.begin(); it != ccf_groups_.end(); ++it) {
      it->second->Validate();
    }
  }
}

std::string RiskAnalysis::CheckAllGates() {
  std::stringstream msg;
  msg << "";  // An empty default message is the indicator of no problems.

  boost::unordered_map<std::string, GatePtr>::iterator it;
  for (it = gates_.begin(); it != gates_.end(); ++it) {
    msg << RiskAnalysis::CheckGate(it->second);
  }

  return msg.str();
}

std::string RiskAnalysis::CheckGate(const GatePtr& event) {
  std::stringstream msg;
  msg << "";  // An empty default message is the indicator of no problems.
  std::string gate = event->type();
  int size = 0;  // The number of children.
  try {
    // This line throws an error if there are no children.
    size = event->children().size();
  } catch (LogicError& err) {
    msg << event->orig_id() << " : No children detected.";
    return msg.str();
  }

  // Gates that should have two or more children.
  std::set<std::string> two_or_more;
  two_or_more.insert("and");
  two_or_more.insert("or");
  two_or_more.insert("nand");
  two_or_more.insert("nor");

  // Gates that should have only one child.
  std::set<std::string> single;
  single.insert("null");
  single.insert("not");

  // Detect inhibit gate.
  if (gate == "and" && event->HasAttribute("flavor")) {
    const Attribute* attr = &event->GetAttribute("flavor");
    if (attr->value == "inhibit") gate = "inhibit";
  }

  // Gate dependent logic.
  if (two_or_more.count(gate)) {
    if (size < 2) {
      boost::to_upper(gate);
      msg << event->orig_id() << " : " << gate << " gate must have 2 or more "
          << "children.\n";
    }
  } else if (single.count(gate)) {
    if (size != 1) {
      boost::to_upper(gate);
      msg << event->orig_id() << " : " << gate
          << " gate must have exactly one child.";
    }
  } else if (gate == "xor") {
    if (size != 2) {
      boost::to_upper(gate);
      msg << event->orig_id() << " : " << gate
          << " gate must have exactly 2 children.\n";
    }
  } else if (gate == "inhibit") {
    msg << RiskAnalysis::CheckInhibitGate(event);

  } else if (gate == "atleast") {
    if (size <= event->vote_number()) {
      boost::to_upper(gate);
      msg << event->orig_id() << " : " << gate
          << " gate must have more children than its vote number "
          << event->vote_number() << ".";
    }
  } else {
    boost::to_upper(gate);
    msg << event->orig_id()
        << " : Gate Check failure. No check for " << gate << " gate.";
  }

  return msg.str();
}

std::string RiskAnalysis::CheckInhibitGate(const GatePtr& event) {
  std::stringstream msg;
  msg << "";  // An empty default message is the indicator of no problems.

  if (event->children().size() != 2) {
    msg << event->orig_id() << " : "
        << "INHIBIT gate must have exactly 2 children.\n";
  } else {
    bool conditional_found = false;
    std::map<std::string, EventPtr> children = event->children();
    std::map<std::string, EventPtr>::iterator it;
    for (it = children.begin(); it != children.end(); ++it) {
      if (primary_events_.count(it->first)) {
        if (it->second->HasAttribute("flavor")) {
          std::string type = it->second->GetAttribute("flavor").value;
          if (type == "conditional") {
            if (!conditional_found) {
              conditional_found = true;
            } else {
              msg << event->orig_id() << " : " << "INHIBIT"
                  << " gate must have exactly one conditional event.\n";
            }
          }
        }
      }
    }
    if (!conditional_found) {
      msg << event->orig_id() << " : " << "INHIBIT"
          << " gate is missing a conditional event.\n";
    }
  }
  return msg.str();
}

std::string RiskAnalysis::CheckMissingEvents() {
  std::string msg = "";
  if (!tbd_house_events_.empty()) {
    msg += "\nMissing definitions for House events:\n";
    boost::unordered_map<std::string, HouseEventPtr>::iterator it;
    for (it = tbd_house_events_.begin(); it != tbd_house_events_.end();
         ++it) {
      msg += it->second->orig_id() + "\n";
    }
  }

  if (!tbd_basic_events_.empty()) {
    msg += "\nMissing definitions for Basic events:\n";
    boost::unordered_map<std::string, BasicEventPtr>::iterator it;
    for (it = tbd_basic_events_.begin(); it != tbd_basic_events_.end();
         ++it) {
      msg += it->second->orig_id() + "\n";
    }
  }

  if (!tbd_events_.empty()) {
    msg += "\nMissing definitions for Untyped events:\n";
    boost::unordered_map<std::string, std::vector<GatePtr> >::iterator it;
    for (it = tbd_events_.begin(); it != tbd_events_.end(); ++it) {
      msg += tbd_orig_ids_.find(it->first)->second + "\n";
    }
  }

  return msg;
}

std::string RiskAnalysis::CheckMissingParameters() {
  std::string msg = "";
  if (!tbd_parameters_.empty()) {
    msg += "\nMissing parameter definitions:\n";
    boost::unordered_map<std::string, ParameterPtr>::iterator it;
    for (it = tbd_parameters_.begin(); it != tbd_parameters_.end();
         ++it) {
      msg += it->first + "\n";
    }
  }

  return msg;
}

void RiskAnalysis::ValidateExpressions() {
  // Check for cyclicity in parameters.
  if (!parameters_.empty()) {
    boost::unordered_map<std::string, ParameterPtr>::iterator it;
    for (it = parameters_.begin(); it != parameters_.end(); ++it) {
      it->second->Validate();
    }
  }

  // Validate expressions.
  if (!expressions_.empty()) {
    try {
      std::set<ExpressionPtr>::iterator it;
      for (it = expressions_.begin(); it != expressions_.end(); ++it) {
        (*it)->Validate();
      }
    } catch (InvalidArgument& err) {
      throw ValidationError(err.msg());
    }
  }

  // Check probability values for primary events.
  if (prob_requested_) {
    std::stringstream msg;
    msg << "";
    if (!ccf_groups_.empty()) {
      std::map<std::string, CcfGroupPtr>::iterator it;
      for (it = ccf_groups_.begin(); it != ccf_groups_.end(); ++it) {
        try {
          it->second->ValidateDistribution();
        } catch (ValidationError& err) {
          msg << it->second->name() << " : " << err.msg() << "\n";
        }
      }
    }
    boost::unordered_map<std::string, BasicEventPtr>::iterator it;
    for (it = basic_events_.begin(); it != basic_events_.end(); ++it) {
      try {
        it->second->Validate();
      } catch (ValidationError& err) {
        msg << it->second->orig_id() << " : " << err.msg() << "\n";
      }
    }
    if (msg.str() != "") {
      std::string head = "Invalid probabilities detected:\n";
      throw ValidationError(head + msg.str());
    }
  }
}

void RiskAnalysis::SetupForAnalysis() {
  // CCF groups.
  if (!ccf_groups_.empty()) {
    std::map<std::string, CcfGroupPtr>::iterator it;
    for (it = ccf_groups_.begin(); it != ccf_groups_.end(); ++it) {
      it->second->ApplyModel();
    }
  }
  // Configure fault trees.
  if (!fault_trees_.empty()) {
    std::map<std::string, FaultTreePtr>::iterator it;
    for (it = fault_trees_.begin(); it != fault_trees_.end(); ++it) {
      it->second->SetupForAnalysis();
    }
  }
}

void RiskAnalysis::GraphingInstructions(std::ostream& out) {
  std::map<std::string, FaultTreePtr>::iterator it;
  for (it = fault_trees_.begin(); it != fault_trees_.end(); ++it) {
    Grapher gr = Grapher();
    gr.GraphFaultTree(it->second, prob_requested_, out);
  }
}

void RiskAnalysis::Report(std::ostream& out) {
  Reporter rp = Reporter();

  // Create XML or use already created document.
  xmlpp::Document* doc = new xmlpp::Document();
  rp.SetupReport(settings_, doc);

  ///@todo Report orphans in XML.
  /// if (!orphan_primary_events_.empty())
  ///   rp.ReportOrphans(orphan_primary_events_, out);

  std::vector<FaultTreeAnalysisPtr>::iterator it;
  for (it = ftas_.begin(); it != ftas_.end(); ++it) {
    rp.ReportFta(*it, doc);
  }

  doc->write_to_stream_formatted(out);
  delete doc;

  if (prob_requested_) {
    std::vector<ProbabilityAnalysisPtr>::iterator it_p;
    for (it_p = prob_analyses_.begin(); it_p != prob_analyses_.end(); ++it_p) {
      rp.ReportProbability(*it_p, out);
    }
    std::vector<UncertaintyAnalysisPtr>::iterator it_u;
    for (it_u = uncertainty_analyses_.begin();
         it_u != uncertainty_analyses_.end(); ++it_u) {
      rp.ReportUncertainty(*it_u, out);
    }
  }
}

}  // namespace scram
