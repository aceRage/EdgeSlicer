
var TargetPage=null;

function OnInit()
{
	TranslatePage();

	TargetPage=GetQueryString("target");
	
	//setTimeout("JumpToTarget()",20*1000);

	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="userguide_loading_page_ready";
	SendWXMessage(JSON.stringify(tSend));
}

function HandleStudio( pVal )
{
	let strCmd=pVal['command'];
	
	if(strCmd=='userguide_profile_load_finish')
	{
		JumpToTarget();
	}
}

function JumpToTarget()
{
	window.open('../'+TargetPage+'/index.html','_self');
}