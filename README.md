# oslabs
start this programm with:

Calc OS ;

tcp/udp client server ; 
server: server 5555 6666 (first: tcp port; second udp port)
client: send short msg: client sendmsg ip:port message or "message with brackets"
        send file to server: client send ip:port filename
        recieve file from server: client recv ip:port filename
        get list of files: client listip:port
        
Treads: Matrix multiplication ; Merge sort ; Dinning Philosophers ; Arrangement of chess pieces ; 

RPC : chat (mailslot) ; calc ( shared memory ) ; transfer files ( pipe ) .
      chat: chat.exe server ms1, where ms1 - name of mailslot.
            chat.exe cli5 ms1, where cli5 - name of client
      calc: calc.exe server mp1
            calc.exe cli1 mp1 5*556
      transfer: transfer.exe server p1
                transfer.exe cli1 p1 filename
