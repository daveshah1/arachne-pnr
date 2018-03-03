/* Copyright (C) 2015 Cotton Seed
   
   This file is part of arachne-pnr.  Arachne-pnr is free software;
   you can redistribute it and/or modify it under the terms of the GNU
   General Public License version 2 as published by the Free Software
   Foundation.
   
   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program. If not, see <http://www.gnu.org/licenses/>. */

#include "util.hh"
#include "netlist.hh"
#include "line_parser.hh"
#include "bitvector.hh"
#include "casting.hh"

#include <cctype>
#include <cstring>
#include <istream>
#include <fstream>
#include <iostream>
#include <string>

class BlifParser : public LineParser
{
  BitVector stobv(const std::string &s_);
  bool radiant_stobv(const std::string &s_, BitVector &out);
public:
  BlifParser(const std::string &f, std::istream &s_)
    : LineParser(f, s_)
  {}
  
  Design *parse();
};

BitVector
BlifParser::stobv(const std::string &s_)
{
  int n = s_.size();
  BitVector bv(n);
  for (int i = 0; i < n; ++i)
    {
      char c = s_[(n - 1) - i];
      if (c == '1')
        bv[i] = true;
      else if (c == '0'
               || c == 'x'
               || c == 'X')

        ;
      else
        fatal("invalid character in integer constant");
    }
  return bv;
}

inline int getNumericValue(char c) {
  c = toupper(c);
  if ((c >= '0') && (c <= '9'))
    return c - '0';
  else if ((c >= 'A') && (c <= 'Z'))
    return (c - 'A') + 10;
  else
    return -1;
}

bool
BlifParser::radiant_stobv(const std::string &s_, BitVector &out) {
  if (s_.length() > 0)
    {
      size_t offset = 0;
      int base = 10;
      int log2base = -1;
      if (s_.size() >= 2)
        {
          if (s_[offset] == '0')
            {
              offset++;
              if (s_[offset] == 'x')
                {
                  base = 16;
                  log2base = 4;
                  offset++;
                }
              else if (s_[offset] == 'b')
                {
                  base = 2;
                  log2base = 1;
                  offset++;
                }
              else
              {
                log2base = 3;
                base = 8;
              }
            }
        }

        if (base == 10)
          {
            uint64_t intval = 0;
            while (offset < s_.length())
              {
                 int cval = getNumericValue(s_[offset]);
                 if (cval == -1)
                   return false;
                 if ((intval * 10 + cval) < intval)
                   fatal("decimal integer overflow in parameter");
                 intval = intval * 10 + cval;
                 offset++;
              }
            out = BitVector(64, intval);
          }
        else
          {
            int n = s_.length() - offset;
            out.resize(n);
            for(int i = 0; i < n; i++)
              {
                int cval = getNumericValue(s_[s_.length() - (1 + i)]);
                if (cval == -1)
                  return false;
                for (int j = 0; j < log2base; j++)
                 {
                   out[i * log2base + j] = (cval & (1 << j)) != 0;
                 }
              }
          }
    } 
  else
    return false;
  return true;
}

Design *
BlifParser::parse()
{
  Design *d = new Design;
  d->create_standard_models();
  
  Model *io_model = d->find_model("SB_IO");
  
  Model *top = nullptr;
  
  std::vector<std::pair<Net *, Net *>> unify;
  
  Instance *inst = nullptr;
  for (;;)
    {
      if (eof())
        goto M;

      // parse current line into words
      read_line();
      
      if (line.empty())
        continue;

      // all directives begin with a dot
      if (line[0] == '.')
        {
        L:
          std::string cmd = words[0];
          if (cmd == ".model")
            {
              if (words.size() != 2)
                fatal(fmt("invalid .model directive: expected exactly 1 argument, got " << words.size()-1));
              if (top)
                fatal("definition of multiple models is not supported");

              top = new Model(d, words[1]);
              d->set_top(top);
            }
          else if (cmd == ".inputs")
            {
              if (!top)
                fatal(".inputs directive outside of model definition");

              for (unsigned i = 1; i < words.size(); i ++)
                {
                  Port *port = top->find_port(words[i]);
                  if (port)
                    {
                      if (port->direction() == Direction::OUT)
                        port->set_direction(Direction::INOUT);
                    }
                  else
                    port = top->add_port(words[i], Direction::IN);
                  Net *net = top->find_or_add_net(words[i]);
                  port->connect(net);
                }
            }
          else if (cmd == ".outputs")
            {
              if (!top)
                fatal(".outputs directive outside of model definition");

              for (unsigned i = 1; i < words.size(); i ++)
                {
                  Port *port = top->find_port(words[i]);
                  if (port)
                    {
                      if (port->direction() == Direction::IN)
                        port->set_direction(Direction::INOUT);
                    }
                  else
                    port = top->add_port(words[i], Direction::OUT);
                  Net *net = top->find_or_add_net(words[i]);
                  port->connect(net);
                }
            }
          else if (cmd == ".names")
            {
              if (!top)
                fatal(".names directive outside of model definition");

              LexicalPosition names_lp = lp;
              
              Net *names_net = nullptr;
              unsigned n = words.size();

              // output is assigned no value; set to zero
              if (n == 2)
                {
                  names_net = top->find_or_add_net(words[1]);
                  names_net->set_is_constant(true);
                  names_net->set_constant(Value::ZERO);
                }

              // output is assigned input; unify nets
              else if (n == 3)
                {
                  unify.push_back(std::make_pair(top->find_or_add_net(words[1]),
                                                 top->find_or_add_net(words[2])));
                }
              else
                fatal(fmt("invalid .names directive: expected 1 or 2 arguments, got " << n-1));

              // parse PLA-style configuration
              bool saw11 = false;
              for (;;)
                {
                  if (eof())
                    {
                      if (n == 3
                          && !saw11)
                        names_lp.fatal("invalid .names directive: unexpected end of file");
                      goto M;
                    }
                  
                  read_line();
                  
                  if (line.empty())
                    continue;
                  
                  if (line[0] == '.')
                    {
                      if (n == 3
                          && !saw11)
                        names_lp.fatal("invalid .names directive: .names entry expected");
                      goto L;
                    }
                  
                  if (words.size() != n - 1)
                    fatal("invalid .names entry: number of gates does not match specified number of nets");
                  
                  // .names + 1 argument
                  if (n == 2)
                    {
                      const std::string &output = words[0];
                      if (output == "1")
                        names_net->set_constant(Value::ONE);
                      else if (output != "0")
                        fatal("invalid .names entry: gate must be either 1 or 0");
                    }
                  else
                    {
                      assert(n == 3);
                      if (words[0] != "1"
                          || words[1] != "1")
                        fatal("invalid .names entry: both gates must be 1 here");
                      saw11 = true;
                    }
                }
            }
          else if (cmd == ".gate")
            {
              if (!top)
                fatal(".gate directive outside of model definition");

              if (words.size() < 2)
                fatal("invalid .gate directive: missing name");
              
              const std::string &n = words[1];
              Model *inst_of = d->find_model(n);
              if (!inst_of)
                fatal(fmt("unknown model `" << n << "'"));
              
              inst = top->add_instance(inst_of);
              
              for (unsigned i = 2; i < words.size(); i ++)
                {
                  const std::string &w = words[i];
                  std::size_t p = w.find('=');
                  if (p == std::string::npos)
                    fatal("invalid formal-actual");
                  
                  std::string formal(w, 0, p),
                    actual(w, p+1);
                  
                  if (actual.empty())
                    continue;
                  
                  Port *port = inst->find_port(formal);
                  if (!port)
                    fatal(fmt("unknown formal `" << formal << "'"));
                  
                  Net *net = top->find_or_add_net(actual);
                  port->connect(net);
                }
            }
          else if (cmd == ".attr")
            {
              if (words.size() != 3)
                fatal(fmt("invalid .attr directive: expected exactly 2 arguments, got " << words.size()-1));
              if (!inst)
                fatal("no gate for .attr directive");
              
              if (words[2][0] == '"')
                {
                  assert(words[2].back() == '"');
                  inst->set_attr(words[1], 
                                 Const(lp, words[2].substr(1, words[2].size() - 2)));
                }
              else
                {
                  inst->set_attr(words[1],
                                 Const(lp, stobv(words[2])));
                }
            }
          else if (cmd == ".param")
            {
              if (words.size() != 3)
                fatal(fmt("invalid .param directive: expected exactly 2 arguments, got " << words.size()-1));
              if (!inst)
                fatal("no gate for .param directive");
              
              if (words[2][0] == '"')
                {
                  assert(words[2].back() == '"');
                  // Radiant uses numeric literals inside strings, so we have to consider them
                  // for compatibility
                  BitVector rad_bit;
                  if(radiant_stobv(words[2].substr(1, words[2].size() - 2), rad_bit))
                    inst->set_param(words[1],
                                    Const(lp, rad_bit));
                  else
                    inst->set_param(words[1], 
                                    Const(lp, words[2].substr(1, words[2].size() - 2)));
                }
              else
                {
                  inst->set_param(words[1],
                                  Const(lp, stobv(words[2])));
                }
            }
          else if (cmd == ".end")
            {
              if (!top)
                fatal(".end directive outside of model definition");

              goto M;
            }
          else
            fatal(fmt("unknown directive '" << cmd << "'"));
        }
      else
        fatal("expected directive");
    }
  
 M:

  if (!top)
    fatal("no top model has been defined");

  // unify
  std::map<Net *, Net *, IdLess> replacement;
  for (const auto &p : unify)
    {
      // n1 drives n2
      Net *n1 = p.first,
        *n2 = p.second;
      
      Net *r = n1;
      while (Net *t = lookup_or_default(replacement, r, nullptr))
        r = t;
      
      Net *x = n1;
      while (x != r)
        {
          auto i = replacement.find(x);
          assert(i != replacement.end());
          
          x = i->second;
          i->second = r;
        }
      
      if (n2 == r)
        fatal(".names cycle\n");
      
      n2->replace(r);

      if (contains(replacement, n2))
        fatal("conflicting .names outputs");

      extend(replacement, n2, r);
    }
  for (const auto &p : replacement)
    {
      Net *n = p.first;
      top->remove_net(n);
      delete n;
    }
  
  for (const auto &p : top->ports())
    {
      if (p.second->is_bidir())
        {
          Net *n = p.second->connection();
          if (n)
            {
              Port *q = p.second->connection_other_port();
              if (!q
                  || !isa<Instance>(q->node())
                  || cast<Instance>(q->node())->instance_of() != io_model
                  || q->name() != "PACKAGE_PIN")
                fatal(fmt("toplevel inout port '" << p.second->name ()
                          << "' not connected to SB_IO PACKAGE_PIN"));
            }
        }
    }
  
  std::set<Net *, IdLess> boundary_nets;
  for (Instance *inst2 : top->instances())
    {
      if (inst2->instance_of() == io_model)
        {
          Port *p = inst2->find_port("PACKAGE_PIN");
          Net *n = p->connection();
          Port *q = p->connection_other_port();
          if (!n
              || !q
              || !isa<Model>(q->node()))
            fatal("SB_IO PACKAGE_PIN not connected to toplevel port");
          
          extend(boundary_nets, n);
        }
    }
  
  for (const auto &p : top->nets())
    {
      Net *n = p.second;
      if (contains(boundary_nets, n))
        continue;
      
      int n_drivers = 0;
      if (n->is_constant())
        ++n_drivers;
      for (Port *p2 : n->connections())
        {
          if (p2->is_output())
            ++n_drivers;
        }
      if (n_drivers > 1)
        fatal(fmt("net `" << n->name() << "' has multiple drivers"));
    }
  
  return d;
}

Design *
read_blif(const std::string &filename)
{
  std::string expanded = expand_filename(filename);
  std::ifstream fs(expanded);
  if (fs.fail())
    fatal(fmt("read_blif: failed to open `" << expanded << "': "
              << strerror(errno)));
  BlifParser parser(filename, fs);
  return parser.parse();
}

Design *
read_blif(const std::string &filename, std::istream &s)
{
  BlifParser parser(filename, s);
  return parser.parse();
}
