#!/usr/bin/python3

import queue
import threading
import time
import socket
import sys
import signal
import datetime
import binascii
import random

from http.server import BaseHTTPRequestHandler, HTTPServer

#  File "/root/interhapd/./interhapd.py", line 364, in neighborhood
#    if dic["bssid"] == words[0]: continue # Remove all but my own entry
#IndexError: list index out of range

class ThreadSafeDict(dict) :
    def __init__(self, * p_arg, ** n_arg) :
        dict.__init__(self, * p_arg, ** n_arg)
        self._lock = threading.Lock()

    def __enter__(self) :
        self._lock.acquire()
        return self

    def __exit__(self, type, value, traceback) :
        self._lock.release()

hostname = socket.gethostname()
remotehostsD = {}
interfacesD = {}
neighborsD = ThreadSafeDict()
beaconrespD = {}
stationsD = {}
threadsD = {}
webservers = [] 
webserverPort = 11112
scriptexiting = False
beaconfilterssid = False

class MyHTTPServer(HTTPServer):
    def __init__(self, server_address, RequestHandlerClass, thread):
        HTTPServer.__init__(self, server_address, RequestHandlerClass)
        self.thread = thread

#table { border-collapse: collapse;}

#td { text-align: center; border: 2px solid #000000; border-style: solid; font-size: 20px; }

htmlstyle = """<style>
table {
  font-family: arial, sans-serif;
  border-collapse: collapse;
  width: 100%;
}

td, th {
  text-align: left;
  padding: 8px;
  font-size: 20px;
}

tr:nth-child(even), thead {
  background-color: #dddddd;
}

table, tr, td, thead {
  border:none;
  vertical-align: middle;
}

ul {
  list-style-type: none;
  margin: 0;
  padding: 0;
  overflow: hidden;
  background-color: #333;
}

li {
  float: left;
  font-size:20px;
}

li a {
  display: block;
  color: white;
  text-align: center;
  padding: 14px 16px;
  text-decoration: none;
  border-right: 3px solid #bbb;
}
li:last-child {
  border-right: none;
}

li a:hover:not(.active) {
  background-color: #111;
}

.active {
  background-color: #04AA6D;
}
</style>
"""

class MyServer(BaseHTTPRequestHandler):
   def log_message(self, format, *args):
      return
   def htmlline(self, line):
      self.wfile.write(bytes(line, "utf-8"))
   def htmllinep(self, line):
      self.wfile.write(bytes("<p>%s</p>" % line, "utf-8"))
   def htmlnavbar(self, tup, tup2, path):
      st = "<ul>"
      i = 0
      for item in tup: 
         cl = "active" if path.startswith(tup2[i]) else "inactive"
         st +="""<li><a class="%s" href="%s">%s</a></li>""" % (cl, tup2[i], item)
         i +=1
      return st + "</ul>"
   def dict2table(self, dic, tup, tup2):
      table = '''<table width="100%" border="5px">\n'''
      table += "<thead>\n"
      table += "<tr>\n"
      for c in tup2: table += "<td>\n%s\n</td>\n" % c
      table += "</tr>\n"
      table += "</thead>\n"
      table += "<tbody>\n"
      for n, rdic in dic.items():
         table += "<tr>\n"
         for c in tup:
            if c == "key": table += "<td>\n%s\n</td>\n" % n
            else:          table += "<td>\n%s\n</td>\n" % rdic[c]
         table += "</tr>\n"
      table += "</tbody>\n"
      table += "</table>\n"
      return table
   def do_GET(self):
      if self.path == "/n":
         with neighborsD:
            footer = "Neighbors: %s" % neighborsD
            table = self.dict2table(neighborsD, ("host", "sock", "ascii_ssid", "key"), \
                                                ("host", "sock", "ssid",       "bssid"))
      elif self.path == "/s":
         footer = "Stations: %s" % stationsD
         table = self.dict2table(stationsD, ("key", "host", "sock"), \
                                          ("bssid", "host", "sock"))
      elif self.path == "/b":
         footer = "Beacons: %s" % beaconrespD
         table = self.dict2table(beaconrespD, ("ssid", "rssi", "station_bssid", "neighbor_bssid"), \
                                              ("ssid", "rssi", "station bssid", "neighbor bssid"))
      elif self.path == "/i":
         footer = "Interfaces: %s" % interfacesD
         table = self.dict2table(interfacesD, ("key", "ssid", "bssid"), \
                                        ("interface", "ssid", "bssid"))
      elif self.path.startswith("/t"):
         stalist = ["key"]
         staname = ["Station bssid"]
         with neighborsD:
            for n, ndic in neighborsD.items():
               stalist.append(n)
               staname.append(ndic["host"] + "<p></p>" + ndic["sock"]+ "<p></p>" + ndic["ascii_ssid"])
            tdic = dict()
            for s, sdic in stationsD.items():
               dic = dict()
               for n, ndic in neighborsD.items():
                  if sdic["host"] == ndic["host"] and sdic["sock"] == ndic["sock"]:
                     dic[n] = "CONNECTED"
                  else:
                     text = "MOVE"
                     key = s + "-" + n
                     nr = ndic["nr"]
                     op = int(nr[20:22], 16)
                     ch = int(nr[22:24], 16)
                     ph = int(nr[24:26], 16)
                     if key in beaconrespD.keys(): text = beaconrespD[key]["rssi"]
                     dic[n] = '''<A HREF=t?tm-%s-%s-%s-%s,0x0000,%s,%s,%s,0301ff>%s</A>''' % \
                       (sdic["host"], sdic["sock"], s, n, op, ch, ph, text)
               tdic[s] = dic
         path = self.path[2:]
         if path.startswith( "?tm-" ):
            tm = path[4:].split("-")
            com = "BSS_TM_REQ %s pref=1 abriged=1 disassoc_imminent=1 disassoc_timer=10 neighbor=%s" %(tm[2], tm[3])
            eprint("TRANSISTION: %s\n%s" % (tm, com))
            il = self.server.thread.docommand(tm[0], tm[1], com)
            if il != None: eprint("TRANSISTION RESPONSE: %s" % (il.remain))

         table = self.dict2table(tdic, stalist, staname)
         footer = "Transistion: %s" % tdic
      else:
         footer= "Remote Hosts: %s" % remotehostsD
         table = ""
         testD = {
            "test1": {'host':"testhost1",'sock':'testsock1'}, \
            "test2": {'host':"testhost2",'sock':'testsock2'}, \
            "test3": {'host':"testhost3",'sock':'testsock3'}, \
            "test4": {'host':"testhost4",'sock':'testsock4'}, \
         }

         table = self.dict2table(testD, ("key", "host", "sock"), \
                                          ("bssid", "host", "sock"))
      navbar = self.htmlnavbar( \
         ("Stations", "Neighborhood", "Interfaces", "Beacons", "Transition"), \
         ("/s"       , "/n"           , "/i"         , "/b"      , "/t") , self.path)

      self.send_response(200)
      self.send_header("Content-type", "text/html")
      self.end_headers()
      self.htmlline("")
      self.htmlline('''
         <html>
         <head>
           <title>interhapd web interface</title>
           <meta http-equiv="refresh" content="5;url=%s">
           <style type="text/css"> %s </style>
         </head>
         ''' % ( self.path[0:2], htmlstyle))
#      self.htmllinep("<head></head>")
      self.htmlline('''
         <body style="font-family:'Courier New', Courier, monospace;">
         %s
         %s
         <p>%s</p>
         </body>
         </html>''' % (navbar, table, footer))
   

def server(thread):
   eprint("Web Server started")
   server = MyHTTPServer(("", webserverPort), MyServer, thread)
   webservers.append(server)
   try:
      server.serve_forever()
   except KeyboardInterrupt:
      pass
   finally:   
      server.server_close()
      webservers.remove(server)
      eprint("Web Server stopped")

def event(thread):
# This thread is only receiving events
   while  True:
      il = thread.getinput(None)
      if il == None: continue
#      eprint("%s processing %s-%s %s" % (thread.name, il.fromhost, il.fromsock, il.remain))
      if il.remain.startswith("<"):
         words = il.remain.lstrip("<").partition('>')[2].split()
         if   words[0] == "AP-STA-CONNECTED":
            threadsD[stations.__name__].signal(il)
         elif words[0] == "AP-STA-DISCONNECTED":
            threadsD[stations.__name__].signal(il)
         elif words[0] == "BEACON-RESP-RX":
            threadsD[beacons.__name__].signal(il)
         elif words[0] == "BSS-TM-RESP":
            pass
      elif il.remain.startswith("INTERHAPD LISTENING STARTED"):
         thread.remotehost_add(il)
         threadsD[neighborhood.__name__].signal(il)
         threadsD[stations.__name__].signal(il)
         eprint("REMOTEHOSTS: %s %s" %(remotehostsD, il.remain))
      elif il.remain.startswith("INTERHAPD LISTENING STOPPED"):
         thread.remotehost_remove(il.fromhost)
         threadsD[neighborhood.__name__].signal(il)
         threadsD[stations.__name__].signal(il)
         eprint("REMOTEHOSTS: %s %s" %(remotehostsD, il.remain))
      elif il.remain.startswith("INTERHAPD INTERFACE STARTED"):
         threadsD[neighborhood.__name__].signal(il)
         threadsD[stations.__name__].signal(il)
      elif il.remain.startswith("INTERHAPD INTERFACE STOPPED"):
         threadsD[neighborhood.__name__].signal(il)
         threadsD[stations.__name__].signal(il)
      elif il.remain.startswith("INTERHAPD NEIGHBORHOOD CHANGED"):
         threadsD[neighborhood.__name__].signal(il)
#      else:
#        eprint("OTHER EVENT: %s-%s %s" % (il.fromhost, il.fromsock, il.remain))


def command(thread):
# This thread is only receiving events
   while  True:
      il = thread.getinput(None)
      if il == None: continue
      if il.remain.startswith("SHOW_MY_NEIGHBOR"):
         eprint("ERIC: %s" % il.fromhost)
         thread.respond( il.fromhost, il.fromsock, thread.get_my_neighbors())

def beacons(thread):
# This thread is only sending commands
   while  True:
      il = thread.wait(3)  # Wait for signal from event thread
      if il != None: # Wait was interupted by a signal from other thread
#         eprint(il.remain)
         words = il.remain.split(" ")
         if len(words[4]) <= 84: continue
         nbssid = words[4][30:32]+":"+words[4][32:34]+":"+words[4][34:36]+ \
             ":"+words[4][36:38]+":"+words[4][38:40]+":"+words[4][40:42]
         beaconrespD[words[1] +"-"+ nbssid] = { \
            'rssi':int(words[4][26:28], 16), \
            'ssid':binascii.unhexlify(words[4][84:84+(int(words[4][80:84],16)*2)]).decode(), \
            'neighbor_bssid':nbssid , \
            'station_bssid':words[1] , \
         }
      else: # Timed out
#         eprint(beaconrespD)
         beaconrespD.clear()
         for sta, sdic in stationsD.items():
            if sdic["host"] != hostname: continue# 
            for interface, idic in interfacesD.items():
               if sdic["sock"] != interface: continue
               btype = 2
               detail = 1
               ssid = idic["ssid"]
               com = "REQ_BEACON %s 510000000000%02xffffffffffff0201%02x" \
                  %(sta, btype, detail )
               if beaconfilterssid == True:
                  com += "%04x" % len(ssid) + ssid.encode('utf-8').hex()
#               eprint(com)
               ill = thread.docommand( hostname, sdic["sock"], com)

def stations(thread):
# This thread is only sending commands
   while  True:
      il = thread.wait(300)  # Wait for signal from event thread
      if il != None: # Wait was interupted by a signal from other thread
#         eprint("%s signal from %s-%s: %s" % (thread.name, il.fromhost, il.fromsock, il.remain))
         if il.remain.startswith("<"):
            words = il.remain.lstrip("<").partition('>')[2].split()
            if   words[0] == "AP-STA-CONNECTED": 
               sta = thread.add_station(words[1], il)
            elif words[0] == "AP-STA-DISCONNECTED":
               sta = words[1]
               if sta in stationsD.keys():
                  if stationsD[sta]["host"] == il.fromhost and stationsD[sta]["sock"] == il.fromsock:
                     stationsD.pop(sta, None)
         elif il.remain.startswith("INTERHAPD INTERFACE STARTED"):
            ill = thread.docommand( il.fromhost, il.fromsock, "STA-FIRST")
            if ill == None: continue
            while ill.remain != "":
               sta = ill.remain.partition("^")[0]
               thread.add_station(sta, ill)
               thread.fakeeventall(il.fromsock, "event", "<3>AP-STA-CONNECTED %s" % sta)
               ill = thread.docommand( il.fromhost, il.fromsock, "STA-NEXT %s" %(sta))
               if ill == None: break
#            eprint("STATIONS: %s " %(stationsD))
         elif il.remain.startswith("INTERHAPD INTERFACE STOPPED"):
            while True:
               removed = False
               for sta, dic in stationsD.items():
                  if stationsD[sta]["sock"] != il.fromsock: continue
                  if stationsD[sta]["host"] != hostname: continue
                  stationsD.pop(sta, None)
                  thread.fakeeventall(il.fromsock, "event", "<3>AP-STA-DISCONNECTED %s" % sta)
                  removed = True
                  break
               if removed == False: break
         elif il.remain.startswith("INTERHAPD LISTENING STARTED"):
            if il.fromhost != hostname:
               for sta, dic in stationsD.items():
                  if stationsD[sta]["host"] != hostname: continue
                  thread.fakeeventall(stationsD[sta]["sock"], "event", "<3>AP-STA-CONNECTED %s" % sta)
         elif il.remain.startswith("INTERHAPD LISTENING STOPPED"):
            while il.fromhost != hostname:
               removed = False
               for sta, dic in stationsD.items():
                  if stationsD[sta]["host"] != hostname: continue
                  stationsD.pop(sta, None)
                  removed = True
                  break
               if removed == False: break
                  

def neighborhood(thread):
# This thread is only sending commands
   while scriptexiting == False:
#      eprint("%s processing from %s-%s: %s" % (thread.name, il.fromhost, il.fromsock, il.remain ))
      il = thread.wait(300)  # Wait for signal from event thread
      if il != None: # Wait was interupted by a signal from other thread
#         eprint("%s signal from %s-%s: %s" % (thread.name, il.fromhost, il.fromsock, il.remain))
         if il.remain.startswith("INTERHAPD INTERFACE STARTED"):
            ill = thread.docommand( il.fromhost, il.fromsock, "GET_CONFIG")
            if ill == None: continue
            if not "=" in ill.remain: continue # empty
            dic = dict(x.split("=") for x in ill.remain.rstrip('^').split("^"))
            ill = thread.repeatcommand( il.fromhost, il.fromsock, "SHOW_NEIGHBOR", "")
            if ill == None: continue
            for line in ill.remain.rstrip('^').split("^"):
               words = line.split()
               if dic["bssid"] != words[0]: continue # If this is the entry for the self interface
               for word in words:
                  if   word.startswith("ssid="): dic["neighbor_ssid"] = word.lstrip("ssid=")
                  elif word.startswith("nr="):   dic["neighbor_nr"]   = word.lstrip("nr=")
            interfacesD[il.fromsock] = dic
            thread.sendeventall("event", "INTERHAPD NEIGHBORHOOD CHANGED")
#            eprint("INTERFACES %s" % (interfacesD))
         elif il.remain.startswith("INTERHAPD INTERFACE STOPPED"):
            interfacesD.pop(il.fromsock, None)
            thread.sendeventall("event","INTERHAPD NEIGHBORHOOD CHANGED")
      eprint("REBIULDING NEIGHBORHOOD")
# clear local neighborhood
      with neighborsD:
         neighborsD.clear()
         for interface, dic in interfacesD.items():
            il = thread.docommand(hostname, interface, "SHOW_NEIGHBOR")
            if il == None: continue
            for line in il.remain.rstrip('^').split("^"):
               words = line.split()
               if len(words) == 0: continue
               if dic["bssid"] == words[0]: continue # Remove all but my own entry
               thread.docommand(hostname, interface, "REMOVE_NEIGHBOR " + words[0])
# build up neigborhood on local interfacesD
         while True:
            removed = False
            for host, hdic in remotehostsD.items():
               age = datetime.datetime.now() - hdic["lastseen"]
               if age < datetime.timedelta(minutes = 10): continue
##### FIX!!!
#               thread.remotehost_remove(host)
               removed = True
               break
            if removed == False: break
         for host, hdic in remotehostsD.items():
            il = thread.docommand(host, "command", "SHOW_MY_NEIGHBOR")
            if il == None: continue
            eprint("TEST %s" % il.remain)
            thread.set_my_neighbors(il.remain)
         thread.set_my_neighbors(thread.get_my_neighbors())

threadfunctionList = [event, command, stations, neighborhood, beacons, server]

def main():
# Initialisation
   signal.signal(signal.SIGTSTP, handler)
   signal.signal(signal.SIGTERM, handler)
   threadID = 0
   for tFunction in threadfunctionList:
      thread = myThread(threadID, tFunction.__name__)
      threadsD[tFunction.__name__] = thread
      thread.start()
      threadID += 1

# Main loop
   while True:
      inputline = input()
#      eprint("INPUT: %s" % (inputline))
      il = myLine(inputline)
      if il.remain == "EXIT!": break
      if il.tohost != hostname and il.tohost != "broadcast": continue
      for tFunction in threadfunctionList:
         if tFunction.__name__ != il.tosock: continue
         threadsD[tFunction.__name__].inputq.put(il)
# Exit cleanup
   global scriptexiting
   scriptexiting = True
   eprint("SCRIPT STOPPING")
   for server in webservers: server.shutdown()
   for t in threadsD.values():
      t.inputq.put("EXIT!")
      t.sleepq.put("EXIT!")
      t.join()
# End of main()




def handler(signum, frame): pass

def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)

class myLine:
   fromhost=""
   fromsock=""
   tohost=""
   tosock=""
   mytype=""
   remain=""
   def __init__(self, line):
      part = line.partition(' ')
      while (part[0] != ""):
         partpart = part[0].partition('=')
         if partpart[0] == "FROM":         
            partpartpart = partpart[2].partition('-')
            self.fromhost=partpartpart[0]
            self.fromsock=partpartpart[2]
         elif partpart[0] == "TO":         
            partpartpart = partpart[2].partition('-')
            self.tohost=partpartpart[0]
            self.tosock=partpartpart[2]
         elif partpart[0].startswith("COMMAND") \
           or partpart[0].startswith("RESPONSE") \
           or partpart[0].startswith("EVENT"):
            self.mytype = partpart[0]
            self.remain = partpart[2]
            if part[2] != "": self.remain += " " + part[2]
            break
         part = part[2].partition(' ')
    
class myThread (threading.Thread):
   def __init__(self, threadID, name):
      threading.Thread.__init__(self)
      self.threadID = threadID
      self.name = name
      self.inputq = queue.Queue()
      self.sleepq = queue.Queue()
   def run(self):
      threadfunctionList[self.threadID] (self)
   def get(self, timeout, q):
      try:
         while True:
            ret = q.get(block=True, timeout=timeout)
            if ret == "EXIT!": sys.exit()
            if scriptexiting: sys.exit()
            else: return ret 
      except queue.Empty: 
         return None
   def getinput(self, timeout):
      return self.get(timeout, self.inputq)
   def wait(self, timeout):
      return self.get(timeout, self.sleepq)
   def docommand(self, tohost, tosock, command):
      hsh = "%08x" % random.getrandbits(32)
      print("FROM=%s-%s TO=%s-%s COMMAND-%s=%s" % (hostname, self.name, tohost, tosock, hsh, command), flush=True)
      i = 0
      while True:
         if i > 10: return None
         ret = self.getinput(1)
         if ret == None: return ret
         part = ret.mytype.partition('-')
#         eprint("hash value: %s %s" % (hsh, part[2]))
         if hsh == part[2]: return ret
         i += 1
   def repeatcommand(self, tohost, tosock, command, repeatstring):
      while True:
         ret = self.docommand(tohost, tosock, command)
         if ret == None: return ret
         if (ret.remain != repeatstring): return ret
         time.sleep(1)
   def respond(self, tohost, tosock, response):
      print("FROM=%s-%s TO=%s-%s RESPONSE=%s" % (hostname, self.name, tohost, tosock, response), flush=True)
      return
   def sendevent(self, tohost, tosock, event):
      print("FROM=%s-%s TO=%s-%s EVENT=%s" % (hostname, self.name, tohost, tosock, event), flush=True)
   def broadcastevent(self, tosock, event):
      print("FROM=%s-%s TO=%s-%s EVENT=%s" % (hostname, self.name, "broadcast", tosock, event), flush=True)
      return
   def fakeevent(self, fromsock, tohost, tosock, event):
      print("FROM=%s-%s TO=%s-%s EVENT=%s" % (hostname, fromsock, tohost, tosock, event), flush=True)
      return
   def sendeventall(self, tosock, event):
      for host, hdic in remotehostsD.items(): self.sendevent(host, tosock, event)
      return
   def fakeeventall(self, fromsock, tosock, event):
      for host, hdic in remotehostsD.items(): self.fakeevent(fromsock, host, tosock, event)
      return
   def signal(self, il):
      self.sleepq.put(il)
      return
   def remotehost_add(self, il):
#      eprint("ADD %s " % il.fromhost)
      if not il.fromhost in remotehostsD:
         for sta, dic in stationsD.items():
            if dic["host"] == hostname:
               self.fakeevent(dic["sock"], il.fromhost, il.fromsock, "<3>AP-STA-CONNECTED %s" % sta) # Send event self
         remotehostsD[il.fromhost] = {'lastseen':datetime.datetime.now(),'b':'bbb','c':'ccc'}
      else:
         remotehostsD[il.fromhost]["lastseen"] = datetime.datetime.now()
   def remotehost_remove(self, host):
      remotehostsD.pop(host, None)
      while True:
         removed = False
         for sta, dic in stationsD.items():
            if dic["host"] != host: continue 
            stationsD.pop(sta, None)
            removed = True
            break
         if removed == False: break
   def add_station(self, sta, il):
      d = dict()
      d["host"] = il.fromhost
      d["sock"] = il.fromsock
      stationsD[sta] = d
      return
   def get_my_neighbors(self):
      line=""
      for interface, dic in interfacesD.items():
         line += "%s ssid=%s ascii_ssid=%s nr=%s host=%s sock=%s^" % (dic["bssid"], \
            dic["neighbor_ssid"], dic["ssid"], dic["neighbor_nr"], hostname, interface)
      return line
   def set_my_neighbor(self, key, dic):
      return "%s ssid=%s nr=%s" %(key, dic["ssid"], dic["nr"])
   def set_my_neighbors(self, lines):
      for line in lines.rstrip('^').split("^"):
         part = line.partition(' ')
         if not "=" in part[2]: continue # empty
         dic = dict(x.split("=") for x in part[2].split(" "))
         neighborsD[part[0]] = dic
         for interface, idic in interfacesD.items():
            if idic["bssid"] == part[0]: continue # Add all but my own entry
            self.docommand(hostname, interface, "SET_NEIGHBOR " + self.set_my_neighbor(part[0], dic))

if __name__ == "__main__": main()
sys.exit()

# Table beacon with wildcard BSSID
# basic_beacon = '51000000000002ffffffffffff020100'

# Table beacon with wildcard BSSID and SSID filter
#beacon_with_ssid = '51000000000002ffffffffffff02010000077373696452524d'

# Passive beacon with wildcard BSSID
#beacon_passive =   '510b0000000000ffffffffffff020100'

# Active beacon with wildcard BSSID
#beacon_active = '510b0000000001ffffffffffff020100'

# Passive beacon with duration set
#beacon_passive_duration = '510b0000c80000ffffffffffff020100'


#hostapd_cli -i XXXX bss_tm_req AA:AA:AA:AA:AA:AA neighbor=-BB:BB:BB:BB:BB:BB abridged=1

#XXXX = The adapter you wish to switch FROM.
#AA:AA... = The MAC of the client you want to transfer over.
#BB:BB... = The MAC of the access point you want to EXCLUDE from possible access points where it will roam to.

# BSS_TM_REQ e0:cc:f8:57:68:60 neighbor=aa:bb:cc:d8:48:08,0x0000,81,7,7 abridged=1


#int ieee802_11_parse_candidate_list(const char *pos, u8 *nei_rep,
#				    size_t nei_rep_len)


#	/*
#	 * BSS Transition Candidate List Entries - Neighbor Report elements
#	 * neighbor=<BSSID>,<BSSID Information>,<Operating Class>,
#	 * <Channel Number>,<PHY Type>[,<hexdump of Optional Subelements>]
#	 */

#  <BSSID Information> = 0x0000

#		val = strtol(pos, &endptr, 0);
#		*nei_pos++ = atoi(pos); /* Operating Class */
#		*nei_pos++ = atoi(pos); /* Channel Number */
#		*nei_pos++ = atoi(pos); /* PHY Type */
# atoi -> decimal  (base 10)

#	/*
#	 * Neighbor Report element size = BSSID + BSSID info + op_class + chan +
#	 * phy type + wide bandwidth channel subelement.
#	 */
#	nr = wpabuf_alloc(ETH_ALEN + 4 + 1 + 1 + 1 + 5);
#  0	wpabuf_put_data(nr, hapd->own_addr, ETH_ALEN);
#  6	wpabuf_put_le32(nr, bssid_info);
# 10	wpabuf_put_u8(nr, op_class);
# 11	wpabuf_put_u8(nr, channel);
# 12	wpabuf_put_u8(nr, ieee80211_get_phy_type(hapd->iface->freq, ht, vht));
# 13	wpabuf_put_u8(nr, WNM_NEIGHBOR_WIDE_BW_CHAN);
# 14	wpabuf_put_u8(nr, 3);
# 15	wpabuf_put_u8(nr, width);
# 16	wpabuf_put_u8(nr, center_freq1_idx);
# 17	wpabuf_put_u8(nr, center_freq2_idx);
# 18
