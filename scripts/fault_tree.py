#!/usr/bin/env python
#
# Copyright (C) 2014-2016 Olzhas Rakhimov
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

"""Fault tree classes and common facilities."""

from collections import deque


class Node(object):
    """Representation of a base class for a node in a fault tree.

    Attributes:
        name: A specific name that identifies this node.
        parents: A set of parents of this node.
    """

    def __init__(self, name):
        """Constructs a new node with a unique name.

        Note that the tracking of parents introduces a cyclic reference.

        Args:
            name: Identifier for the node.
        """
        self.name = name
        self.parents = set()

    def is_common(self):
        """Indicates if this node appears in several places."""
        return len(self.parents) > 1

    def is_orphan(self):
        """Determines if the node is parentless."""
        return not self.parents

    def num_parents(self):
        """Returns the number of unique parents."""
        return len(self.parents)

    def add_parent(self, gate):
        """Adds a gate as a parent of the node.

        Args:
            gate: The gate where this node appears.
        """
        assert gate not in self.parents
        self.parents.add(gate)


class BasicEvent(Node):
    """Representation of a basic event in a fault tree.

    Attributes:
        prob: Probability of failure of this basic event.
    """

    def __init__(self, name, prob):
        """Initializes a basic event node.

        Args:
            name: Identifier of the node.
            prob: Probability of the basic event.
        """
        super(BasicEvent, self).__init__(name)
        self.prob = prob

    def to_xml(self):
        """Produces OpenPSA MEF XML definition of the basic event."""
        return ("<define-basic-event name=\"" + self.name + "\">\n"
                "<float value=\"" + str(self.prob) + "\"/>\n"
                "</define-basic-event>\n")

    def to_shorthand(self):
        """Produces the shorthand definition of the basic event."""
        return "p(" + self.name + ") = " + str(self.prob) + "\n"


class HouseEvent(Node):
    """Representation of a house event in a fault tree.

    Attributes:
        state: State of the house event ("true" or "false").
    """

    def __init__(self, name, state):
        """Initializes a house event node.

        Args:
            name: Identifier of the node.
            state: Boolean state string of the constant.
        """
        super(HouseEvent, self).__init__(name)
        self.state = state

    def to_xml(self):
        """Produces OpenPSA MEF XML definition of the house event."""
        return ("<define-house-event name=\"" + self.name + "\">\n"
                "<constant value=\"" + self.state + "\"/>\n"
                "</define-house-event>\n")

    def to_shorthand(self):
        """Produces the shorthand definition of the house event."""
        return "s(" + self.name + ") = " + str(self.state) + "\n"


class Gate(Node):
    """Representation of a fault tree gate.

    Attributes:
        operator: Logical operator of this formula.
        k_num: Min number for the combination operator.
        g_arguments: arguments that are gates.
        b_arguments: arguments that are basic events.
        h_arguments: arguments that are house events.
        u_arguments: arguments that are undefined.
        mark: Marking for various algorithms like toposort.
    """

    def __init__(self, name, operator, k_num=None):
        """Initializes a gate.

        Args:
            name: Identifier of the node.
            operator: Boolean operator of this formula.
            k_num: Min number for the combination operator.
        """
        super(Gate, self).__init__(name)
        self.mark = None
        self.operator = operator
        self.k_num = k_num
        self.g_arguments = set()
        self.b_arguments = set()
        self.h_arguments = set()
        self.u_arguments = set()

    def num_arguments(self):
        """Returns the number of arguments."""
        return (len(self.b_arguments) + len(self.h_arguments) +
                len(self.g_arguments) + len(self.u_arguments))

    def add_argument(self, argument):
        """Adds argument into a collection of gate arguments.

        Note that this function also updates the parent set of the argument.

        Args:
            argument: Gate, HouseEvent, BasicEvent, or Node argument.
        """
        argument.parents.add(self)
        if isinstance(argument, Gate):
            self.g_arguments.add(argument)
        elif isinstance(argument, BasicEvent):
            self.b_arguments.add(argument)
        elif isinstance(argument, HouseEvent):
            self.h_arguments.add(argument)
        else:
            assert isinstance(argument, Node)
            self.u_arguments.add(argument)

    def get_ancestors(self):
        """Collects ancestors from this gate.

        Returns:
            A set of ancestors.
        """
        ancestors = set([self])
        parents = deque(self.parents)
        while parents:
            parent = parents.popleft()
            if parent not in ancestors:
                ancestors.add(parent)
                parents.extend(parent.parents)
        return ancestors

    def to_xml(self, nest=0):
        """Produces OpenPSA MEF XML definition of the gate.

        Args:
            nest: The level for nesting formulas of argument gates.
        """
        def convert_formula(gate, nest):
            """Converts the formula of a gate into XML representation."""
            mef_xml = ""
            if gate.operator != "null":
                mef_xml += "<" + gate.operator
                if gate.operator == "atleast":
                    mef_xml += " min=\"" + str(gate.k_num) + "\""
                mef_xml += ">\n"
            for h_arg in gate.h_arguments:
                mef_xml += "<house-event name=\"" + h_arg.name + "\"/>\n"

            for b_arg in gate.b_arguments:
                mef_xml += "<basic-event name=\"" + b_arg.name + "\"/>\n"

            for u_arg in gate.u_arguments:
                mef_xml += "<event name=\"" + u_arg.name + "\"/>\n"

            if nest > 0:
                for g_arg in gate.g_arguments:
                    mef_xml += convert_formula(g_arg, nest - 1)

            else:
                for g_arg in gate.g_arguments:
                    mef_xml += "<gate name=\"" + g_arg.name + "\"/>\n"

            if gate.operator != "null":
                mef_xml += "</" + gate.operator + ">\n"
            return mef_xml

        mef_xml = "<define-gate name=\"" + self.name + "\">\n"
        mef_xml += convert_formula(self, nest)
        mef_xml += "</define-gate>\n"
        return mef_xml


class CcfGroup(object):
    """Representation of CCF groups in a fault tree.

    Attributes:
        name: The name of an instance CCF group.
        members: A collection of members in a CCF group.
        prob: Probability for a CCF group.
        model: The CCF model chosen for a group.
        factors: The factors of the CCF model.
    """

    def __init__(self, name):
        """Constructs a unique CCF group with a unique name.

        Args:
            name: Identifier for the group.
        """
        self.name = name
        self.members = []
        self.prob = None
        self.model = None
        self.factors = None

    def to_xml(self):
        """Produces OpenPSA MEF XML definition of the CCF group."""
        mef_xml = ("<define-CCF-group name=\"" + self.name + "\""
                   " model=\"" + self.model + "\">\n<members>\n")
        for member in self.members:
            mef_xml += "<basic-event name=\"" + member.name + "\"/>\n"
        mef_xml += ("</members>\n<distribution>\n<float value=\"" +
                    str(self.prob) + "\"/>\n</distribution>\n")
        mef_xml += "<factors>\n"
        assert self.model == "MGL"
        level = 2
        for factor in self.factors:
            mef_xml += ("<factor level=\"" + str(level) + "\">\n"
                        "<float value=\"" + str(factor) + "\"/>\n</factor>\n")
            level += 1

        mef_xml += "</factors>\n</define-CCF-group>\n"
        return mef_xml


def toposort_gates(root_gates, gates):
    """Sorts gates topologically starting from the root gate.

    The gate marks are used for the algorithm.
    After this sorting the marks are reset to None.

    Args:
        root_gates: The root gates of the graph.
        gates: Gates to be sorted.

    Returns:
        A deque of sorted gates.
    """
    for gate in gates:
        gate.mark = ""

    def visit(gate, final_list):
        """Recursively visits the given gate sub-tree to include into the list.

        Args:
            gate: The current gate.
            final_list: A deque of sorted gates.
        """
        assert gate.mark != "temp"
        if not gate.mark:
            gate.mark = "temp"
            for arg in gate.g_arguments:
                visit(arg, final_list)
            gate.mark = "perm"
            final_list.appendleft(gate)

    sorted_gates = deque()
    for root_gate in root_gates:
        visit(root_gate, sorted_gates)
    assert len(sorted_gates) == len(gates)
    for gate in gates:
        gate.mark = None
    return sorted_gates
